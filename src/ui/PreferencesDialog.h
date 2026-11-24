#pragma once
#include "AppState.h"
#include <QDialog>
class KeyboardPrefsPage;
class QTabWidget;
class QLineEdit;

class QSpinBox;
class QCheckBox;
class QComboBox;
class ColorButton;

// Edit > Preferences (Fireworks: General / Editing tabs, plus Themes).
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr, int initialTab = 0);
    Prefs prefs() const;
    void setPrefs(const Prefs &p);
    void reject() override;        // Cancel also undoes the live theme preview
    // Themes > Custom from colour (the buttons prompt, then call these; tests call them directly)
    QString saveCustomAs(const QString &name);          // new named theme from the current seed; returns its id
    bool renameCustom(const QString &name);             // the selected named theme
    bool deleteCustom();                                // the selected named theme (back to "custom")
    bool importThemeFile(const QString &path, QString *error = nullptr);
    bool exportThemeFile(const QString &path, QString *error = nullptr) const;
    class PressureCurveEdit *tabletCurve() const { return m_curve; }
    class TabletTestArea *tabletTest() const { return m_tabTest; }
    static QVector<QPointF> curvePreset(int i);   // 0 Linear, 1 Soft, 2 Firm
private:
    void syncCurvePreset();
    class PressureCurveEdit *m_curve = nullptr;
    class TabletTestArea *m_tabTest = nullptr;
    QComboBox *m_curvePreset = nullptr;
    QSpinBox *m_tabMinSize = nullptr, *m_tabMinOpacity = nullptr;
    QCheckBox *m_tabEraser = nullptr;
    void previewTheme();           // live apply of the Themes tab
    void syncPasteboard();
    void fillThemeCombo(const QString &select);
    theme::Spec *selectedSpec();   // the generated theme being edited (custom or named), or null
    void loadSeedControls();       // selected theme -> seed controls
    void seedEdited();             // seed controls -> selected theme, live preview
    void updateSeedPreview();
    QString uniqueName(const QString &name, int skip = -1) const;
    theme::Spec m_custom;
    QVector<theme::Spec> m_user;
    class QGroupBox *m_customBox = nullptr;
    ColorButton *m_seed = nullptr;
    class QLineEdit *m_seedHex = nullptr;
    QComboBox *m_seedMode = nullptr;
    class QSlider *m_vibrancy = nullptr;
    class QLabel *m_vibLabel = nullptr, *m_swatches = nullptr, *m_contrastNote = nullptr;
    class QPushButton *m_saveAs = nullptr, *m_rename = nullptr, *m_delete = nullptr, *m_import = nullptr, *m_export = nullptr;
    QTabWidget *m_tabs = nullptr;
    KeyboardPrefsPage *m_keyboard = nullptr;
    QCheckBox *m_carryToolSettings = nullptr;
    QCheckBox *m_pasteAtMouse = nullptr;
    QCheckBox *m_useSystemIcons = nullptr;
    QCheckBox *m_useSystemFileDialogs = nullptr;
    QCheckBox *m_enableWebFeatures = nullptr;
    QCheckBox *m_useSystemFonts = nullptr;
    QCheckBox *m_useCustomFonts = nullptr;
    QLineEdit *m_fontDir = nullptr;
    class QPushButton *m_fontBrowse = nullptr;
    void exportSettings();
    void importSettings();

    Prefs m_original;
    bool m_loading = false;
    QComboBox *m_theme;
    QCheckBox *m_followSystem, *m_accentOverride, *m_pasteboardOverride;
    ColorButton *m_accent;
    class QComboBox *m_toneMap = nullptr;
    class QDoubleSpinBox *m_exposure = nullptr;
    QSpinBox *m_undo, *m_undoMem, *m_fitMargin, *m_pick, *m_grid, *m_recent, *m_flyoutHold, *m_shiftFine = nullptr, *m_ctrlSnap = nullptr;
    QComboBox *m_interp, *m_sliderOrient = nullptr;
    QCheckBox *m_highlight, *m_brushCursors, *m_precise, *m_checker, *m_canvasBorder, *m_canvasShadow, *m_snapCanvas, *m_snapObjects;
    ColorButton *m_defStroke, *m_defFill, *m_highlightColor, *m_selColor, *m_pasteboard, *m_gridColor;
};
