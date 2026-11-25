#pragma once

class QComboBox;
class QCoreApplication;
class QWidget;

// App-wide closed-combo sizing: wide enough to show item text, capped so
// layouts (especially the Property Inspector) do not overflow. Longer items
// are middle-elided with a full-text tooltip; the popup may be wider.
// The file-type dropdown keeps its own FileDialogs behaviour.
namespace combofit {

constexpr int kDefaultCap = 280;
constexpr int kInspectorCap = 200;

void install(QCoreApplication *app = nullptr);
void apply(QComboBox *combo);
void applyTree(QWidget *root);
bool isFileTypeCombo(const QComboBox *combo);
// theme::apply: skip closed-combo refit on Polish/StyleChange (font is unchanged).
void setSuspended(bool on);

} // namespace combofit
