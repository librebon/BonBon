#include <QDoubleSpinBox>
#include "PopupSlider.h"
#include "PreferencesDialog.h"
#include "Brand.h"
#include "Theme.h"
#include "KeyboardPrefs.h"
#include "Shortcuts.h"
#include "MainWindow.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include "core/PixelScale.h"
#include "Dialogs.h"
#include "TabletPrefs.h"
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QFile>
#include <QFileDialog>
#include "FileDialogs.h"
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QSlider>

PreferencesDialog::PreferencesDialog(QWidget *parent, int initialTab) : QDialog(parent) {
    setObjectName(QStringLiteral("PreferencesDialog"));
    setWindowTitle(QStringLiteral("Preferences"));
    auto *v = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    auto *tabs = m_tabs;
    tabs->setObjectName(QStringLiteral("prefsTabs"));
    v->addWidget(tabs);
    auto spin = [](const char *name, int lo, int hi, const QString &suffix = {}) {
        auto *s = new PopupSliderField; s->setObjectName(QLatin1String(name)); s->setRange(lo, hi); s->setSuffix(suffix); return s;
    };
    auto color = [](const char *name, bool alpha = false) {
        auto *b = new ColorButton(nullptr, alpha); b->setObjectName(QLatin1String(name)); b->setFixedSize(40, 22); return b;
    };

    // General
    auto *gen = new QWidget;
    auto *gf = new QFormLayout(gen);
    m_undo = spin("prefUndoSteps", 1, 999);
    gf->addRow(QStringLiteral("Undo steps:"), m_undo);
    m_undoMem = spin("prefUndoMemory", 64, 65536);
    m_undoMem->setSingleStep(256);
    m_undoMem->setSuffix(QStringLiteral(" MB"));
    m_undoMem->setToolTip(QStringLiteral("Bitmap memory the undo history of one document may use. Past it, the oldest steps are dropped (the last step can always be undone)."));
    gf->addRow(QStringLiteral("Undo memory:"), m_undoMem);
    m_defStroke = color("prefDefaultStroke");
    m_defFill = color("prefDefaultFill");
    gf->addRow(QStringLiteral("Default stroke color:"), m_defStroke);
    gf->addRow(QStringLiteral("Default fill color:"), m_defFill);
    m_highlightColor = color("prefHighlightColor");
    gf->addRow(QStringLiteral("Mouse highlight color:"), m_highlightColor);
    m_selColor = color("prefSelectionColor");
    gf->addRow(QStringLiteral("Selection color:"), m_selColor);
    m_interp = new QComboBox; m_interp->setObjectName(QStringLiteral("prefInterpolation"));
    m_interp->addItems(pixelscale::resampleNames());
    m_interp->setToolTip(QStringLiteral("Default Scaling for new bitmaps (created, imported, pasted or dropped).\n"
        "Existing bitmaps keep their own method (Property Inspector → Scaling). Image Size, Export and\n"
        "Batch Scale can still pick a resample explicitly. Pixel-art scalers enlarge by whole factors\n"
        "and resample down for in-between sizes; shrinking uses Nearest Neighbor."));
    gf->addRow(QStringLiteral("Interpolation:"), m_interp);
    m_recent = spin("prefRecentFiles", 0, 30);
    gf->addRow(QStringLiteral("Recent files:"), m_recent);
    m_toneMap = new QComboBox; m_toneMap->setObjectName(QStringLiteral("prefHdrToneMap"));
    m_toneMap->addItems({QStringLiteral("Clamp"), QStringLiteral("Reinhard"), QStringLiteral("ACES filmic"), QStringLiteral("Auto exposure")});
    m_toneMap->setToolTip(QStringLiteral("How high-dynamic-range images (OpenEXR, Radiance HDR, PFM) become 8-bit when opened"));
    gf->addRow(QStringLiteral("HDR tone mapping:"), m_toneMap);
    m_exposure = new PopupSliderDoubleField; m_exposure->setObjectName(QStringLiteral("prefHdrExposure"));
    m_exposure->setRange(-10, 10); m_exposure->setSingleStep(0.5); m_exposure->setDecimals(1); m_exposure->setSuffix(QStringLiteral(" EV"));
    gf->addRow(QStringLiteral("HDR exposure:"), m_exposure);
    m_carryToolSettings = new QCheckBox(QStringLiteral("Carry tool settings between documents"));
    m_carryToolSettings->setObjectName(QStringLiteral("prefCarryToolSettings"));
    m_carryToolSettings->setToolTip(QStringLiteral("When on, the last-used tool, colours and brush options continue into new documents and are remembered across sessions. When off (default), every new canvas starts from the built-in defaults and each .lwd keeps its own settings."));
    gf->addRow(m_carryToolSettings);
    m_pasteAtMouse = new QCheckBox(QStringLiteral("Paste at mouse location"));
    m_pasteAtMouse->setObjectName(QStringLiteral("prefPasteAtMouse"));
    m_pasteAtMouse->setToolTip(QStringLiteral("When on, Paste (Edit menu or shortcut) centres the pasted content on the mouse pointer if it is over the canvas. If the pointer is elsewhere, placement is unchanged. Paste in Place is not affected."));
    gf->addRow(m_pasteAtMouse);
    m_useSystemIcons = new QCheckBox(QStringLiteral("Use system icon theme"));
    m_useSystemIcons->setObjectName(QStringLiteral("prefUseSystemIcons"));
    m_useSystemIcons->setToolTip(QStringLiteral("When off (default), %1 uses its bundled GNOME-Wine icons, hue-shifted to match the current interface theme. When on, toolbar and menu icons come from the desktop icon theme.").arg(brand::name()));
    gf->addRow(m_useSystemIcons);
    m_useSystemFileDialogs = new QCheckBox(QStringLiteral("Use system file dialogs"));
    m_useSystemFileDialogs->setObjectName(QStringLiteral("prefUseSystemFileDialogs"));
    m_useSystemFileDialogs->setToolTip(QStringLiteral("When off (default), %1 uses its own themed file dialog (the same one the AppImage shows). When on, the desktop's native file dialog is used.").arg(brand::name()));
    m_useSystemFileDialogs->setChecked(false);
    gf->addRow(m_useSystemFileDialogs);
    m_enableWebFeatures = new QCheckBox(QStringLiteral("Enable web features"));
    m_enableWebFeatures->setObjectName(QStringLiteral("prefEnableWebFeatures"));
    m_enableWebFeatures->setToolTip(QStringLiteral("When off (default), the Web tools section, Insert > Slice and Insert > Hotspot, hotspot tools, link / alt / target, rollover and swap behaviours, button-symbol editing, and File > Export HTML / Export Slices / Export Image Map are hidden. Slices already in a document stay visible on the canvas, in Layers, and in the Property Inspector, and View > Show Slices still toggles their overlay. Documents that already contain slices, hotspots, behaviours or buttons still load, keep that data, and export it unchanged. Turning this on restores the hidden controls immediately, with no restart."));
    m_enableWebFeatures->setChecked(false);
    gf->addRow(m_enableWebFeatures);
    // Apply immediately so Font Folder / Import Theme / Export Settings opened
    // from this dialog already honour the new mode; Cancel restores m_original.
    connect(m_useSystemFileDialogs, &QCheckBox::toggled, this, [this](bool on) {
        Prefs live = appState().prefs;
        live.useSystemFileDialogs = on;
        appState().setPrefs(live);
    });
    connect(m_enableWebFeatures, &QCheckBox::toggled, this, [this](bool on) {
        if (m_loading) return;
        Prefs live = appState().prefs;
        live.enableWebFeatures = on;
        appState().setPrefs(live);
    });
    {
        auto *box = new QGroupBox(QStringLiteral("Fonts"));
        box->setObjectName(QStringLiteral("prefFontsBox"));
        auto *fl = new QFormLayout(box);
        m_useSystemFonts = new QCheckBox(QStringLiteral("Use system fonts"));
        m_useSystemFonts->setObjectName(QStringLiteral("prefUseSystemFonts"));
        m_useSystemFonts->setChecked(true);
        fl->addRow(m_useSystemFonts);
        m_useCustomFonts = new QCheckBox(QStringLiteral("Use custom fonts"));
        m_useCustomFonts->setObjectName(QStringLiteral("prefUseCustomFonts"));
        fl->addRow(m_useCustomFonts);
        auto *row = new QHBoxLayout;
        m_fontDir = new QLineEdit;
        m_fontDir->setObjectName(QStringLiteral("prefCustomFontDir"));
        m_fontDir->setPlaceholderText(QStringLiteral("Folder of .ttf / .otf / .ttc / .pfb fonts"));
        m_fontBrowse = new QPushButton(QStringLiteral("Browse..."));
        m_fontBrowse->setObjectName(QStringLiteral("prefFontBrowse"));
        row->addWidget(m_fontDir, 1);
        row->addWidget(m_fontBrowse);
        fl->addRow(QStringLiteral("Font folder:"), row);
        gf->addRow(box);
        auto syncFont = [this] {
            const bool on = m_useCustomFonts->isChecked();
            m_fontDir->setEnabled(on);
            m_fontBrowse->setEnabled(on);
        };
        connect(m_useCustomFonts, &QCheckBox::toggled, this, syncFont);
        connect(m_useSystemFonts, &QCheckBox::toggled, this, [this](bool on) {
            if (m_loading) return;
            if (!on && !m_useCustomFonts->isChecked()) {
                QMessageBox::warning(this, QStringLiteral("Fonts"), QStringLiteral("At least one font source must stay on; system fonts will remain enabled."));
                m_useSystemFonts->setChecked(true);
            }
        });
        connect(m_useCustomFonts, &QCheckBox::toggled, this, [this](bool on) {
            if (m_loading) return;
            if (!on && !m_useSystemFonts->isChecked()) {
                QMessageBox::warning(this, QStringLiteral("Fonts"), QStringLiteral("At least one font source must stay on; system fonts will remain enabled."));
                m_useSystemFonts->setChecked(true);
            }
        });
        connect(m_fontBrowse, &QPushButton::clicked, this, [this] {
            const QString d = filedialogs::getExistingDirectory(this, QStringLiteral("Font Folder"), m_fontDir->text());
            if (!d.isEmpty()) m_fontDir->setText(d);
        });
        syncFont();
    }
    tabs->addTab(gen, QStringLiteral("General"));

    // Editing
    auto *ed = new QWidget;
    auto *ef = new QFormLayout(ed);
    m_highlight = new QCheckBox(QStringLiteral("Mouse highlight")); m_highlight->setObjectName(QStringLiteral("prefMouseHighlight"));
    m_brushCursors = new QCheckBox(QStringLiteral("Brush-size painting cursors")); m_brushCursors->setObjectName(QStringLiteral("prefBrushCursors"));
    m_precise = new QCheckBox(QStringLiteral("Precise cursors")); m_precise->setObjectName(QStringLiteral("prefPreciseCursors"));
    m_checker = new QCheckBox(QStringLiteral("Checkerboard behind transparent canvas")); m_checker->setObjectName(QStringLiteral("prefCheckerboard"));
    ef->addRow(m_highlight);
    ef->addRow(m_brushCursors);
    ef->addRow(m_precise);
    ef->addRow(m_checker);
    m_canvasShadow = new QCheckBox(QStringLiteral("Canvas drop shadow"));
    m_canvasShadow->setObjectName(QStringLiteral("prefCanvasShadow"));
    m_canvasShadow->setToolTip(QStringLiteral("Draw a soft shadow behind the document (canvas and previews). Off: the document sits flat on the pasteboard."));
    ef->addRow(m_canvasShadow);
    m_canvasBorder = new QCheckBox(QStringLiteral("Canvas border"));
    m_canvasBorder->setObjectName(QStringLiteral("prefCanvasBorder"));
    m_canvasBorder->setToolTip(QStringLiteral("Draw a subtle 1px neutral line around the document (in the canvas and the previews). Off: no border."));
    ef->addRow(m_canvasBorder);
    m_fitMargin = new PopupSliderField;
    m_fitMargin->setObjectName(QStringLiteral("prefFitMargin"));
    m_fitMargin->setRange(0, 200);
    m_fitMargin->setSuffix(QStringLiteral(" px"));
    {
        const QString z = shortcuts::Manager::get().displayShortcutText(QStringLiteral("view.fit"));
        m_fitMargin->setToolTip(QStringLiteral("Space Fit All%1 leaves round the document. 0: the document fills the view exactly on the limiting side.")
            .arg(z.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(z)));
    }
    ef->addRow(QStringLiteral("Fit All margin:"), m_fitMargin);
    m_snapCanvas = new QCheckBox(QStringLiteral("Snap to canvas edges"));
    m_snapCanvas->setObjectName(QStringLiteral("prefSnapCanvas"));
    m_snapCanvas->setToolTip(QStringLiteral("Dragged / resized objects and the crop box snap to the canvas edges and centre lines (6 screen px). Hold Ctrl to suspend."));
    ef->addRow(m_snapCanvas);
    m_snapObjects = new QCheckBox(QStringLiteral("Snap to objects (smart guides)"));
    m_snapObjects->setObjectName(QStringLiteral("prefSnapObjects"));
    m_snapObjects->setToolTip(QStringLiteral("Dragged / resized objects snap to other objects' edges and centres. Hold Ctrl to suspend."));
    ef->addRow(m_snapObjects);
    m_pick = spin("prefPickDistance", 1, 20, QStringLiteral(" px"));
    ef->addRow(QStringLiteral("Pick distance:"), m_pick);
    m_grid = spin("prefGridSize", 1, 500, QStringLiteral(" px"));
    ef->addRow(QStringLiteral("Grid size:"), m_grid);
    m_gridColor = color("prefGridColor", true);
    ef->addRow(QStringLiteral("Grid color:"), m_gridColor);
    m_flyoutHold = spin("prefFlyoutHold", 50, 2000, QStringLiteral(" ms"));
    ef->addRow(QStringLiteral("Tool flyout hold delay:"), m_flyoutHold);
    m_sliderOrient = new QComboBox;
    m_sliderOrient->setObjectName(QStringLiteral("prefSliderOrientation"));
    m_sliderOrient->addItems({QStringLiteral("Horizontal"), QStringLiteral("Vertical")});
    m_sliderOrient->setToolTip(QStringLiteral("Direction of the slider that pops up from the small triangle of value fields (opacity, sizes, ...). Vertical: drag up to increase, like Fireworks."));
    ef->addRow(QStringLiteral("Pop-up slider orientation:"), m_sliderOrient);
    m_shiftFine = spin("prefShiftFineSpeed", 5, 100, QStringLiteral("%"));
    m_shiftFine->setToolTip(QStringLiteral("Speed of a slider drag while Shift is held, as a percentage of the normal speed. "
                                           "Also sets the Shift+wheel and Shift+arrow steps of decimal fields (never below the field's smallest unit)."));
    ef->addRow(QStringLiteral("Shift fine control speed:"), m_shiftFine);
    m_ctrlSnap = spin("prefCtrlSnapIncrement", 1, 100);
    m_ctrlSnap->setToolTip(QStringLiteral("When Ctrl is held, sliders and value fields snap to multiples of this step, "
                                         "in the slider's own units (percent for alpha and opacity, pixels for size, degrees for hue)."));
    ef->addRow(QStringLiteral("Ctrl snap increment:"), m_ctrlSnap);
    tabs->addTab(ed, QStringLiteral("Editing"));

    // Themes
    auto *th = new QWidget;
    auto *tf = new QFormLayout(th);
    m_theme = new QComboBox;
    m_theme->setObjectName(QStringLiteral("prefTheme"));
    fillThemeCombo(theme::defaultId());
    tf->addRow(QStringLiteral("Theme:"), m_theme);
    m_followSystem = new QCheckBox(QStringLiteral("Follow system light / dark setting"));
    m_followSystem->setObjectName(QStringLiteral("prefThemeFollowSystem"));
    m_followSystem->setToolTip(QStringLiteral("Use a dark theme while the desktop prefers dark and a light one while it prefers light "
                                              "(the chosen theme when it already matches)."));
    tf->addRow(m_followSystem);
    m_accentOverride = new QCheckBox(QStringLiteral("Custom accent color:"));
    m_accentOverride->setObjectName(QStringLiteral("prefAccentOverride"));
    m_accent = color("prefAccentColor");
    tf->addRow(m_accentOverride, m_accent);
    m_pasteboardOverride = new QCheckBox(QStringLiteral("Custom pasteboard color:"));
    m_pasteboardOverride->setObjectName(QStringLiteral("prefPasteboardOverride"));
    m_pasteboard = color("prefPasteboardColor");
    tf->addRow(m_pasteboardOverride, m_pasteboard);
    // Custom from colour: one seed colour -> a whole theme (OKLCH tonal palettes, enforced contrast)
    m_customBox = new QGroupBox(QStringLiteral("Custom from colour"));
    m_customBox->setObjectName(QStringLiteral("prefCustomGroup"));
    auto *cf = new QFormLayout(m_customBox);
    m_seed = color("prefSeedColor");
    m_seedHex = new QLineEdit;
    m_seedHex->setObjectName(QStringLiteral("prefSeedHex"));
    m_seedHex->setMaximumWidth(80);
    m_seedHex->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("#?[0-9A-Fa-f]{0,6}")), m_seedHex));
    m_seedHex->setToolTip(QStringLiteral("Seed colour as #RRGGBB"));
    auto *seedRow = new QHBoxLayout;
    seedRow->addWidget(m_seed);
    seedRow->addWidget(m_seedHex);
    seedRow->addStretch();
    cf->addRow(QStringLiteral("Seed colour:"), seedRow);
    m_seedMode = new QComboBox;
    m_seedMode->setObjectName(QStringLiteral("prefSeedMode"));
    m_seedMode->addItems({QStringLiteral("Auto (from the seed's lightness)"), QStringLiteral("Light"), QStringLiteral("Dark")});
    cf->addRow(QStringLiteral("Mode:"), m_seedMode);
    m_vibrancy = new QSlider(Qt::Horizontal);
    m_vibrancy->setObjectName(QStringLiteral("prefSeedVibrancy"));
    m_vibrancy->setRange(0, 100);
    m_vibrancy->setToolTip(QStringLiteral("How strongly the seed tints the neutral surfaces (0: grey, 100: strongly tinted)"));
    PopupSliderHub::adoptSlider(m_vibrancy);
    m_vibLabel = new QLabel;
    m_vibLabel->setMinimumWidth(34);
    auto *vibRow = new QHBoxLayout;
    vibRow->addWidget(m_vibrancy, 1);
    vibRow->addWidget(m_vibLabel);
    cf->addRow(QStringLiteral("Vibrancy:"), vibRow);
    m_swatches = new QLabel;
    m_swatches->setObjectName(QStringLiteral("prefSeedSwatches"));
    cf->addRow(QStringLiteral("Preview:"), m_swatches);
    m_contrastNote = new QLabel;
    m_contrastNote->setObjectName(QStringLiteral("prefSeedContrast"));
    m_contrastNote->setProperty("lwNote", true);
    cf->addRow(m_contrastNote);
    auto *btnRow = new QHBoxLayout;
    auto mkBtn = [&](const char *name, const QString &text, const QString &tip) {
        auto *b = new QPushButton(text); b->setObjectName(QLatin1String(name)); b->setToolTip(tip); b->setAutoDefault(false); btnRow->addWidget(b); return b;
    };
    m_saveAs = mkBtn("prefThemeSaveAs", QStringLiteral("Save As..."), QStringLiteral("Save the generated theme under a name"));
    m_rename = mkBtn("prefThemeRename", QStringLiteral("Rename..."), QStringLiteral("Rename the selected custom theme"));
    m_delete = mkBtn("prefThemeDelete", QStringLiteral("Delete"), QStringLiteral("Delete the selected custom theme"));
    m_import = mkBtn("prefThemeImport", QStringLiteral("Import..."), QStringLiteral("Add a theme from a .json theme file"));
    m_export = mkBtn("prefThemeExport", QStringLiteral("Export..."), QStringLiteral("Write the generated theme to a .json theme file"));
    btnRow->addStretch();
    cf->addRow(btnRow);
    tf->addRow(m_customBox);
    connect(m_seed, &ColorButton::colorPicked, this, [this](const QColor &c) {
        if (m_loading) return;
        m_loading = true; m_seedHex->setText(c.name().toUpper()); m_loading = false;
        seedEdited();
    });
    connect(m_seedHex, &QLineEdit::textEdited, this, [this](const QString &t) {
        QString h = t.trimmed();
        if (!h.startsWith(QLatin1Char('#'))) h.prepend(QLatin1Char('#'));
        if (h.size() != 7 || !QColor::isValidColorName(h)) return;
        m_seed->setColor(QColor::fromString(h));
        seedEdited();
    });
    connect(m_seedMode, &QComboBox::currentIndexChanged, this, [this] { seedEdited(); });
    connect(m_vibrancy, &QSlider::valueChanged, this, [this](int val) { m_vibLabel->setText(QStringLiteral("%1%").arg(val)); seedEdited(); });
    connect(m_saveAs, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString n = QInputDialog::getText(this, QStringLiteral("Save Theme"), QStringLiteral("Theme name:"), QLineEdit::Normal,
                                                uniqueName(QStringLiteral("My Theme")), &ok);
        if (ok && !n.trimmed().isEmpty()) saveCustomAs(n);
    });
    connect(m_rename, &QPushButton::clicked, this, [this] {
        const theme::Spec *s = selectedSpec();
        if (!s || m_theme->currentData().toString() == theme::customId()) return;
        bool ok = false;
        const QString n = QInputDialog::getText(this, QStringLiteral("Rename Theme"), QStringLiteral("Theme name:"), QLineEdit::Normal, s->name, &ok);
        if (ok && !n.trimmed().isEmpty()) renameCustom(n);
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        const theme::Spec *s = selectedSpec();
        if (!s || m_theme->currentData().toString() == theme::customId()) return;
        if (QMessageBox::question(this, QStringLiteral("Delete Theme"), QStringLiteral("Delete the theme \"%1\"?").arg(s->name)) == QMessageBox::Yes)
            deleteCustom();
    });
    connect(m_import, &QPushButton::clicked, this, [this] {
        const QString f = filedialogs::getOpenFileName(this, QStringLiteral("Import Theme"), QString(), QStringLiteral("%1 themes (*.json);;All files (*)").arg(brand::name()));
        QString err;
        if (!f.isEmpty() && !importThemeFile(f, &err)) QMessageBox::warning(this, QStringLiteral("Import Theme"), err);
    });
    connect(m_export, &QPushButton::clicked, this, [this] {
        const theme::Spec *s = selectedSpec();
        if (!s) return;
        QString f = filedialogs::getSaveFileName(this, QStringLiteral("Export Theme"), s->name + QStringLiteral(".json"), QStringLiteral("%1 themes (*.json)").arg(brand::name()));
        if (f.isEmpty()) return;
        if (!f.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) f += QStringLiteral(".json");
        QString err;
        if (!exportThemeFile(f, &err)) QMessageBox::warning(this, QStringLiteral("Export Theme"), err);
    });

    auto *note = new QLabel(QStringLiteral("Changes preview immediately; Cancel restores the previous theme."));
    note->setObjectName(QStringLiteral("prefThemeNote"));
    note->setWordWrap(true);
    note->setEnabled(false);
    tf->addRow(note);
    tabs->addTab(th, QStringLiteral("Themes"));
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this] { loadSeedControls(); syncPasteboard(); previewTheme(); updateSeedPreview(); });
    connect(m_followSystem, &QCheckBox::toggled, this, [this] { syncPasteboard(); previewTheme(); });
    connect(m_accentOverride, &QCheckBox::toggled, this, [this](bool on) { m_accent->setEnabled(on); previewTheme(); });
    connect(m_accent, &ColorButton::colorPicked, this, [this] { m_accentOverride->setChecked(true); previewTheme(); });
    connect(m_pasteboardOverride, &QCheckBox::toggled, this, [this] { syncPasteboard(); previewTheme(); });
    connect(m_pasteboard, &ColorButton::colorPicked, this, [this] { m_pasteboardOverride->setChecked(true); previewTheme(); });

    // ---- Tablet (v0.3.1): pressure curve, minimums, eraser end, test area
    {
        auto *tb = new QWidget;
        tb->setObjectName(QStringLiteral("prefTabletTab"));
        auto *tl = new QHBoxLayout(tb);
        auto *left = new QVBoxLayout;
        auto *cl = new QLabel(QStringLiteral("Pressure curve (pen pressure \u2192 tool):"));
        left->addWidget(cl);
        m_curve = new PressureCurveEdit;
        left->addWidget(m_curve, 1);
        auto *presetRow = new QHBoxLayout;
        m_curvePreset = new QComboBox;
        m_curvePreset->setObjectName(QStringLiteral("prefTabletPreset"));
        m_curvePreset->addItems({QStringLiteral("Linear"), QStringLiteral("Soft"), QStringLiteral("Firm"), QStringLiteral("Custom")});
        presetRow->addWidget(new QLabel(QStringLiteral("Preset:")));
        presetRow->addWidget(m_curvePreset, 1);
        left->addLayout(presetRow);
        tl->addLayout(left, 1);
        auto *right = new QVBoxLayout;
        auto *tfl = new QFormLayout;
        m_tabMinSize = new PopupSliderField;
        m_tabMinSize->setObjectName(QStringLiteral("prefTabletMinSize"));
        m_tabMinSize->setRange(0, 100);
        m_tabMinSize->setSuffix(QStringLiteral(" %"));
        m_tabMinSize->setToolTip(QStringLiteral("Brush size at the lightest touch, as a share of the tool's size"));
        tfl->addRow(QStringLiteral("Minimum size:"), m_tabMinSize);
        m_tabMinOpacity = new PopupSliderField;
        m_tabMinOpacity->setObjectName(QStringLiteral("prefTabletMinOpacity"));
        m_tabMinOpacity->setRange(0, 100);
        m_tabMinOpacity->setSuffix(QStringLiteral(" %"));
        m_tabMinOpacity->setToolTip(QStringLiteral("Opacity / flow / strength at the lightest touch"));
        tfl->addRow(QStringLiteral("Minimum opacity:"), m_tabMinOpacity);
        m_tabEraser = new QCheckBox(QStringLiteral("Eraser end switches to the Eraser tool"));
        m_tabEraser->setObjectName(QStringLiteral("prefTabletEraser"));
        tfl->addRow(m_tabEraser);
        right->addLayout(tfl);
        auto *hint = new QLabel(QStringLiteral("What the pressure changes is set per tool in the Properties panel "
                                               "(Pressure: Size / Opacity / Flow). A mouse presses at 100%."));
        hint->setWordWrap(true);
        hint->setEnabled(false);
        right->addWidget(hint);
        m_tabTest = new TabletTestArea([this] {
            return TabletTestArea::Settings{m_curve->curve(), m_tabMinSize->value() / 100.0, m_tabMinOpacity->value() / 100.0};
        });
        right->addWidget(m_tabTest, 1);
        auto *clear = new QPushButton(QStringLiteral("Clear"));
        clear->setObjectName(QStringLiteral("prefTabletClear"));
        right->addWidget(clear, 0, Qt::AlignRight);
        tl->addLayout(right, 1);
        tabs->addTab(tb, QStringLiteral("Tablet"));
        connect(clear, &QPushButton::clicked, m_tabTest, &TabletTestArea::clear);
        connect(m_tabTest, &TabletTestArea::rawPressure, m_curve, &PressureCurveEdit::setMarker);
        connect(m_curvePreset, &QComboBox::activated, this, [this](int i) {
            if (i < 3) m_curve->setCurve(curvePreset(i));
        });
        connect(m_curve, &PressureCurveEdit::curveChanged, this, [this] { syncCurvePreset(); });
    }

    m_keyboard = new KeyboardPrefsPage(qobject_cast<MainWindow *>(parent), this);
    tabs->addTab(m_keyboard, QStringLiteral("Keyboard"));

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    auto *exportBtn = bb->addButton(QStringLiteral("Export Settings..."), QDialogButtonBox::ActionRole);
    exportBtn->setObjectName(QStringLiteral("prefExportSettings"));
    auto *importBtn = bb->addButton(QStringLiteral("Import Settings..."), QDialogButtonBox::ActionRole);
    importBtn->setObjectName(QStringLiteral("prefImportSettings"));
    connect(exportBtn, &QPushButton::clicked, this, &PreferencesDialog::exportSettings);
    connect(importBtn, &QPushButton::clicked, this, &PreferencesDialog::importSettings);

    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        Prefs d;
        d.theme = theme::defaultId();
        d.userThemes = m_user;   // defaults never throw away saved themes
        setPrefs(d);
        previewTheme();
    });
    v->addWidget(bb);
    m_original = appState().prefs;
    setPrefs(appState().prefs);
    if (initialTab > 0 && initialTab < m_tabs->count()) m_tabs->setCurrentIndex(initialTab);
}

void PreferencesDialog::fillThemeCombo(const QString &select) {
    const bool was = m_loading;
    m_loading = true;
    m_theme->clear();
    for (const theme::Info &i : theme::list()) m_theme->addItem(i.name, i.id);
    m_theme->insertSeparator(m_theme->count());
    m_theme->addItem(QStringLiteral("Custom from colour"), theme::customId());
    for (const theme::Spec &s : m_user) m_theme->addItem(s.name, theme::userId(s.name));
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(select)));
    m_loading = was;
}

theme::Spec *PreferencesDialog::selectedSpec() {
    const QString id = m_theme->currentData().toString();
    if (id == theme::customId()) return &m_custom;
    if (id.startsWith(QLatin1String("user:")))
        for (theme::Spec &s : m_user) if (theme::userId(s.name) == id) return &s;
    return nullptr;
}

void PreferencesDialog::loadSeedControls() {
    if (!m_customBox) return;
    const theme::Spec *s = selectedSpec();
    const bool named = s && s != &m_custom;
    const bool was = m_loading;
    m_loading = true;
    const theme::Spec shown = s ? *s : m_custom;
    m_seed->setColor(shown.seed);
    m_seedHex->setText(shown.seed.name().toUpper());
    m_seedMode->setCurrentIndex(qBound(0, shown.mode, 2));
    m_vibrancy->setValue(shown.vibrancy);
    m_vibLabel->setText(QStringLiteral("%1%").arg(shown.vibrancy));
    m_loading = was;
    for (QWidget *wd : {static_cast<QWidget *>(m_seed), static_cast<QWidget *>(m_seedHex), static_cast<QWidget *>(m_seedMode),
                        static_cast<QWidget *>(m_vibrancy), static_cast<QWidget *>(m_saveAs), static_cast<QWidget *>(m_export)})
        wd->setEnabled(s);
    m_rename->setEnabled(named);
    m_delete->setEnabled(named);
    m_customBox->setTitle(named ? QStringLiteral("Custom from colour: %1").arg(s->name) : QStringLiteral("Custom from colour"));
}

void PreferencesDialog::seedEdited() {
    if (m_loading) return;
    theme::Spec *s = selectedSpec();
    if (!s) return;
    s->seed = m_seed->color().toRgb();
    s->seed.setAlpha(255);
    s->mode = m_seedMode->currentIndex();
    s->vibrancy = m_vibrancy->value();
    theme::setGenerated(m_custom, m_user);
    syncPasteboard();
    previewTheme();
    updateSeedPreview();
}

void PreferencesDialog::updateSeedPreview() {
    if (!m_swatches) return;
    theme::setGenerated(m_custom, m_user);
    const theme::Spec *s = selectedSpec();
    const theme::Spec sp = s ? *s : m_custom;
    const auto r = theme::generate(sp);
    const char *keys[] = {"window", "body", "input", "tools", "accent", "text", "frame", "pasteboard", "tipBg"};
    QImage img(9 * 22, 18, QImage::Format_RGB32);
    img.fill(r.value(QStringLiteral("window")));
    {
        QPainter p(&img);
        int x = 0;
        for (const char *k : keys) {
            p.fillRect(QRect(x, 0, 21, 18), r.value(QLatin1String(k)));
            p.setPen(r.value(QStringLiteral("frame")));
            p.drawRect(QRect(x, 0, 20, 17));
            x += 22;
        }
    }
    m_swatches->setPixmap(QPixmap::fromImage(img));
    m_swatches->setToolTip(QStringLiteral("window, panel, input, tools, accent, text, border, pasteboard, tooltip"));
    auto C = [&](const char *a, const char *b) { return theme::contrast(r.value(QLatin1String(a)), r.value(QLatin1String(b))); };
    m_contrastNote->setText(QStringLiteral("%1 theme. Text %2:1, borders %3:1, accent text %4:1")
                                .arg(theme::specIsDark(sp) ? QStringLiteral("Dark") : QStringLiteral("Light"))
                                .arg(C("text", "window"), 0, 'f', 1).arg(C("frame", "window"), 0, 'f', 1).arg(C("accentText", "accent"), 0, 'f', 1));
    m_swatches->setEnabled(s);
    m_contrastNote->setEnabled(s);
}

QString PreferencesDialog::uniqueName(const QString &nameIn, int skip) const {
    const QString base = nameIn.trimmed().isEmpty() ? QStringLiteral("Custom Theme") : nameIn.trimmed().remove(QLatin1Char('@'));
    auto taken = [&](const QString &n) {
        for (int i = 0; i < m_user.size(); ++i) if (i != skip && m_user[i].name.compare(n, Qt::CaseInsensitive) == 0) return true;
        for (const theme::Info &i : theme::list()) if (i.name.compare(n, Qt::CaseInsensitive) == 0) return true;
        return n.compare(QLatin1String("Custom from colour"), Qt::CaseInsensitive) == 0;
    };
    QString n = base;
    for (int k = 2; taken(n); ++k) n = QStringLiteral("%1 %2").arg(base).arg(k);
    return n;
}

QString PreferencesDialog::saveCustomAs(const QString &name) {
    const theme::Spec *s = selectedSpec();
    theme::Spec copy = s ? *s : m_custom;
    copy.name = uniqueName(name);
    m_user.append(copy);
    theme::setGenerated(m_custom, m_user);
    const QString id = theme::userId(copy.name);
    fillThemeCombo(id);
    loadSeedControls(); syncPasteboard(); previewTheme(); updateSeedPreview();
    return id;
}

bool PreferencesDialog::renameCustom(const QString &name) {
    theme::Spec *s = selectedSpec();
    if (!s || s == &m_custom || name.trimmed().isEmpty()) return false;
    const int idx = int(s - m_user.data());
    s->name = uniqueName(name, idx);
    theme::setGenerated(m_custom, m_user);
    const QString id = theme::userId(s->name);
    fillThemeCombo(id);
    loadSeedControls(); syncPasteboard(); previewTheme(); updateSeedPreview();
    return true;
}

bool PreferencesDialog::deleteCustom() {
    theme::Spec *s = selectedSpec();
    if (!s || s == &m_custom) return false;
    m_user.removeAt(int(s - m_user.data()));
    theme::setGenerated(m_custom, m_user);
    fillThemeCombo(theme::customId());
    loadSeedControls(); syncPasteboard(); previewTheme(); updateSeedPreview();
    return true;
}

bool PreferencesDialog::importThemeFile(const QString &path, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = QStringLiteral("Cannot read %1").arg(path); return false; }
    theme::Spec sp;
    if (!theme::fromJson(f.readAll(), &sp, error)) return false;
    sp.name = uniqueName(sp.name);
    m_user.append(sp);
    theme::setGenerated(m_custom, m_user);
    fillThemeCombo(theme::userId(sp.name));
    loadSeedControls(); syncPasteboard(); previewTheme(); updateSeedPreview();
    return true;
}

bool PreferencesDialog::exportThemeFile(const QString &path, QString *error) const {
    const theme::Spec *s = const_cast<PreferencesDialog *>(this)->selectedSpec();
    if (!s) { if (error) *error = QStringLiteral("Select Custom from colour or a saved custom theme to export"); return false; }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(theme::toJson(*s)) < 0) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    return true;
}

void PreferencesDialog::syncPasteboard() {
    if (m_loading || m_pasteboardOverride->isChecked()) return;
    theme::setGenerated(m_custom, m_user);
    m_pasteboard->setColor(theme::pasteboard(theme::resolve(m_theme->currentData().toString(), m_followSystem->isChecked())));
}

void PreferencesDialog::previewTheme() {
    if (m_loading) return;
    // only the Themes tab previews live; everything else waits for OK
    Prefs p = appState().prefs;
    const Prefs t = prefs();
    p.theme = t.theme;
    p.themeFollowSystem = t.themeFollowSystem;
    p.accentOverride = t.accentOverride;
    p.accentColor = t.accentColor;
    p.pasteboardOverride = t.pasteboardOverride;
    p.pasteboardColor = t.pasteboardColor;
    p.customSeed = t.customSeed;
    p.customMode = t.customMode;
    p.customVibrancy = t.customVibrancy;
    p.userThemes = t.userThemes;
    appState().setPrefs(p);
}

void PreferencesDialog::reject() {
    appState().setPrefs(m_original);
    QDialog::reject();
}

void PreferencesDialog::setPrefs(const Prefs &p) {
    m_undo->setValue(p.undoSteps);
    m_undoMem->setValue(p.undoMemoryMB);
    m_defStroke->setColor(p.defaultStroke);
    m_defFill->setColor(p.defaultFill);
    m_highlightColor->setColor(p.highlightColor);
    m_selColor->setColor(p.selectionColor);
    m_loading = true;
    m_custom = theme::Spec();
    m_custom.seed = p.customSeed;
    m_custom.mode = p.customMode;
    m_custom.vibrancy = p.customVibrancy;
    m_user = p.userThemes;
    theme::setGenerated(m_custom, m_user);
    fillThemeCombo(p.theme);
    loadSeedControls();
    m_followSystem->setChecked(p.themeFollowSystem);
    m_accentOverride->setChecked(p.accentOverride);
    m_accent->setColor(p.accentColor);
    m_accent->setEnabled(p.accentOverride);
    m_pasteboardOverride->setChecked(p.pasteboardOverride);
    m_pasteboard->setColor(p.pasteboardOverride ? p.pasteboardColor : theme::pasteboard(theme::resolve(p.theme, p.themeFollowSystem)));
    m_loading = false;
    updateSeedPreview();
    m_interp->setCurrentIndex(p.interpolation);
    m_recent->setValue(p.recentFiles);
    m_toneMap->setCurrentIndex(qBound(0, p.hdrToneMap, 3));
    m_exposure->setValue(p.hdrExposure);
    m_highlight->setChecked(p.mouseHighlight);
    m_brushCursors->setChecked(p.brushSizeCursors);
    m_precise->setChecked(p.preciseCursors);
    m_checker->setChecked(p.checkerboard);
    m_canvasBorder->setChecked(p.canvasBorder);
    m_fitMargin->setValue(p.fitMargin);
    m_canvasShadow->setChecked(p.canvasShadow);
    m_snapCanvas->setChecked(p.snapToCanvas);
    m_snapObjects->setChecked(p.snapToObjects);
    m_pick->setValue(p.pickDistance);
    m_grid->setValue(p.gridSize);
    m_gridColor->setColor(p.gridColor);
    m_flyoutHold->setValue(p.flyoutHoldMs);
    m_sliderOrient->setCurrentIndex(qBound(0, p.sliderOrientation, 1));
    m_shiftFine->setValue(p.shiftFineSpeed);
    m_ctrlSnap->setValue(p.ctrlSnapIncrement);
    m_curve->setCurve(p.tabletCurve);
    syncCurvePreset();
    m_tabMinSize->setValue(p.tabletMinSize);
    m_tabMinOpacity->setValue(p.tabletMinOpacity);
    m_tabEraser->setChecked(p.tabletEraser);
    if (m_carryToolSettings) m_carryToolSettings->setChecked(p.carryToolSettings);
    if (m_pasteAtMouse) m_pasteAtMouse->setChecked(p.pasteAtMouse);
    if (m_useSystemIcons) m_useSystemIcons->setChecked(p.useSystemIcons);
    if (m_useSystemFileDialogs) m_useSystemFileDialogs->setChecked(p.useSystemFileDialogs);
    {
        const bool was = m_loading;
        m_loading = true;
        if (m_enableWebFeatures) m_enableWebFeatures->setChecked(p.enableWebFeatures);
        m_loading = was;
    }
    {
        const bool was = m_loading;
        m_loading = true;
        // Custom first so turning system fonts off cannot see both boxes as unchecked.
        m_useCustomFonts->setChecked(p.useCustomFonts);
        m_useSystemFonts->setChecked(p.useSystemFonts);
        m_loading = was;
    }
    m_fontDir->setText(p.customFontDir);
    m_fontDir->setEnabled(p.useCustomFonts);
    m_fontBrowse->setEnabled(p.useCustomFonts);
}

QVector<QPointF> PreferencesDialog::curvePreset(int i) {
    switch (i) {
    case 1: return {QPointF(0, 0), QPointF(0.35, 0.6), QPointF(1, 1)};   // Soft: a light touch goes further
    case 2: return {QPointF(0, 0), QPointF(0.6, 0.35), QPointF(1, 1)};   // Firm: press harder for the same result
    default: return tablet::defaultCurve();
    }
}

void PreferencesDialog::syncCurvePreset() {
    const QVector<QPointF> c = m_curve->curve();
    int found = 3;
    for (int i = 0; i < 3; ++i) if (tablet::normalizedCurve(curvePreset(i)) == c) found = i;
    m_curvePreset->setCurrentIndex(found);
}

Prefs PreferencesDialog::prefs() const {
    Prefs p;
    p.undoSteps = m_undo->value();
    p.undoMemoryMB = m_undoMem->value();
    p.defaultStroke = m_defStroke->color();
    p.defaultFill = m_defFill->color();
    p.highlightColor = m_highlightColor->color();
    p.selectionColor = m_selColor->color();
    p.theme = m_theme->currentData().toString();
    p.themeFollowSystem = m_followSystem->isChecked();
    p.accentOverride = m_accentOverride->isChecked();
    p.accentColor = m_accent->color();
    p.customSeed = m_custom.seed;
    p.customMode = m_custom.mode;
    p.customVibrancy = m_custom.vibrancy;
    p.userThemes = m_user;
    p.pasteboardColor = m_pasteboard->color();
    // a pasteboard colour that differs from the theme's is an override even if the box was not ticked
    p.pasteboardOverride = m_pasteboardOverride->isChecked()
        || p.pasteboardColor != theme::pasteboard(theme::resolve(p.theme, p.themeFollowSystem));
    p.interpolation = m_interp->currentIndex();
    p.recentFiles = m_recent->value();
    p.hdrToneMap = m_toneMap->currentIndex();
    p.hdrExposure = m_exposure->value();
    p.mouseHighlight = m_highlight->isChecked();
    p.brushSizeCursors = m_brushCursors->isChecked();
    p.preciseCursors = m_precise->isChecked();
    p.checkerboard = m_checker->isChecked();
    p.canvasBorder = m_canvasBorder->isChecked();
    p.fitMargin = m_fitMargin->value();
    p.canvasShadow = m_canvasShadow->isChecked();
    p.snapToCanvas = m_snapCanvas->isChecked();
    p.snapToObjects = m_snapObjects->isChecked();
    p.pickDistance = m_pick->value();
    p.gridSize = m_grid->value();
    p.gridColor = m_gridColor->color();
    p.flyoutHoldMs = m_flyoutHold->value();
    p.sliderOrientation = m_sliderOrient->currentIndex();
    p.shiftFineSpeed = m_shiftFine->value();
    p.ctrlSnapIncrement = m_ctrlSnap->value();
    p.tabletCurve = m_curve->curve();
    p.tabletMinSize = m_tabMinSize->value();
    p.tabletMinOpacity = m_tabMinOpacity->value();
    p.tabletEraser = m_tabEraser->isChecked();
    if (m_carryToolSettings) p.carryToolSettings = m_carryToolSettings->isChecked();
    if (m_pasteAtMouse) p.pasteAtMouse = m_pasteAtMouse->isChecked();
    if (m_useSystemIcons) p.useSystemIcons = m_useSystemIcons->isChecked();
    if (m_useSystemFileDialogs) p.useSystemFileDialogs = m_useSystemFileDialogs->isChecked();
    if (m_enableWebFeatures) p.enableWebFeatures = m_enableWebFeatures->isChecked();
    p.useSystemFonts = m_useSystemFonts->isChecked();
    p.useCustomFonts = m_useCustomFonts->isChecked();
    p.customFontDir = m_fontDir->text().trimmed();
    return p;
}


void PreferencesDialog::exportSettings() {
    const QString path = filedialogs::getSaveFileName(this, QStringLiteral("Export Settings"),
        brand::nameLower() + QStringLiteral(".lwprefs"), QStringLiteral("%1 preferences (*.lwprefs);;INI (*.ini)").arg(brand::name()));
    if (path.isEmpty()) return;
    // apply current dialog values first
    appState().setPrefs(prefs());
    QSettings out(path, QSettings::IniFormat);
    out.clear();
    // dump the app prefs group
    {
        QSettings st;
        st.beginGroup(QStringLiteral("prefs"));
        for (const QString &k : st.childKeys()) out.setValue(QStringLiteral("prefs/") + k, st.value(k));
        st.endGroup();
    }
    appState().savePrefs();
    // re-read after save
    {
        QSettings st;
        st.beginGroup(QStringLiteral("prefs"));
        for (const QString &k : st.childKeys()) out.setValue(QStringLiteral("prefs/") + k, st.value(k));
        st.endGroup();
    }
    shortcuts::Manager::get().save(out);
    {
        QSettings st;
        if (st.contains(QStringLiteral("carriedToolEnv")))
            out.setValue(QStringLiteral("carriedToolEnv"), st.value(QStringLiteral("carriedToolEnv")));
    }
    out.sync();
    QMessageBox::information(this, QStringLiteral("Export Settings"), QStringLiteral("Saved preferences to %1").arg(path));
}

void PreferencesDialog::importSettings() {
    const QString path = filedialogs::getOpenFileName(this, QStringLiteral("Import Settings"),
        {}, QStringLiteral("%1 preferences (*.lwprefs);;INI (*.ini);;All (*)").arg(brand::name()));
    if (path.isEmpty()) return;
    QSettings in(path, QSettings::IniFormat);
    if (in.status() != QSettings::NoError && in.allKeys().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import Settings"), QStringLiteral("Could not read %1").arg(path));
        return;
    }
    // copy into app settings
    QSettings st;
    for (const QString &k : in.allKeys()) {
        if (k.startsWith(QLatin1String("prefs/")) || k.startsWith(QLatin1String("shortcuts/"))
            || k == QLatin1String("carriedToolEnv"))
            st.setValue(k, in.value(k));
    }
    st.sync();
    appState().loadPrefs();
    shortcuts::Manager::get().load(st);
    setPrefs(appState().prefs);
    if (m_keyboard) { m_keyboard->refillSets(); m_keyboard->refillTree(); }
    if (auto *mw = qobject_cast<MainWindow *>(parentWidget())) shortcuts::Manager::get().apply(mw);
    QMessageBox::information(this, QStringLiteral("Import Settings"), QStringLiteral("Imported preferences from %1").arg(path));
}
