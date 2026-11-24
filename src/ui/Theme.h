#pragma once
#include <QColor>
#include <QMap>
#include <QString>
#include <QVector>
class QApplication;
struct Prefs;
namespace theme {
// Interface themes. "classic" is the light grey Macromedia MX look (classic Windows style plus a
// stylesheet); every other theme fills the same stylesheet template with its own colour roles.
struct Info { QString id, name; bool dark; };
QVector<Info> list();                       // the built-in themes, Macromedia Classic first
QString defaultId();                        // "classic" (Libreworks) or "bonbon" (Bonbon build)
bool exists(const QString &id);
// Colour roles of a theme (text, window, accent, accentText, disabled, input, pasteboard, ...).
// accent: optional override of the theme's accent / highlight colour.
QMap<QString, QColor> roles(const QString &id, const QColor &accent = QColor());
QColor pasteboard(const QString &id);
// Follow system: a light theme while the desktop prefers light, a dark one while it prefers dark.
// scheme: Qt::ColorScheme value (0 unknown, 1 light, 2 dark); -1 asks the platform (or the test override).
QString resolve(const QString &id, bool followSystem, int scheme = -1);
void setSystemSchemeOverride(int scheme);   // tests / --color-scheme: -1 back to the platform
int systemScheme();
// WCAG 2 contrast ratio (1..21).
double contrast(const QColor &a, const QColor &b);

// ---- Themes generated from a single seed colour (Preferences > Themes > Custom from colour) ----
// The seed is converted to OKLCH; tonal palettes (neutral, neutral variant, accent; Material 3
// style) give every role, then text / icon / border lightness is pushed until WCAG contrast holds
// (text >= 7:1 on window and inputs, >= 4.5:1 elsewhere; borders, icons and the accent >= 3:1),
// so any seed (pure yellow, black, white, neon) yields a usable theme.
// Ids: "custom" (the live seed in Prefs), "user:<name>" (saved named themes),
// "seed:#rrggbb[:auto|light|dark[:vibrancy]]" (ad hoc, e.g. --theme on the command line).
// Follow system appends "@light" / "@dark" to a generated id to force its mode.
enum class SeedMode { Auto = 0, Light = 1, Dark = 2 };
struct Spec {
    QString name = QStringLiteral("Custom");
    QColor seed = QColor(0xC2, 0x3B, 0x7A);
    int mode = 0;          // SeedMode
    int vibrancy = 40;     // 0..100: how strongly the seed tints the neutral surfaces
};
struct Oklch { double L, C, h; };            // L 0..1, C ~0..0.37, h degrees
Oklch toOklch(const QColor &c);
QColor fromOklch(double L, double C, double h); // gamut-mapped (chroma reduced) into sRGB
bool specIsDark(const Spec &s);              // Auto: dark when the seed's OKLCH lightness < 0.55
QMap<QString, QColor> generate(const Spec &s, const QColor &accent = QColor());
bool isGenerated(const QString &id);
QString customId();                          // "custom"
QString userId(const QString &name);         // "user:<name>"
// The live custom seed and the saved named themes (kept in Prefs; AppState::setPrefs syncs them).
void setGenerated(const Spec &custom, const QVector<Spec> &user);
void syncGenerated(const Prefs &p);
bool specFor(const QString &id, Spec *out);  // custom / user / seed ids (with an @mode suffix)
// Theme files: {"format":"libreworks-theme","version":1,"name","seed","mode","vibrancy","roles":{...}}
QByteArray toJson(const Spec &s);
bool fromJson(const QByteArray &json, Spec *out, QString *error = nullptr);

void apply(QApplication &app);              // Macromedia Classic
void apply(QApplication &app, const QString &id, const QColor &accent = QColor());
int proxyStyleCount();                       // live NoTabFocusStyle instances (tests: must stay 1)
void applyPrefs(const Prefs &p);            // resolves follow-system and the accent override; no-op if unchanged
QString current();                          // id of the applied theme
QColor color(const char *role);             // role of the applied theme (custom painted widgets)
}
