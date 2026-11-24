#pragma once
#include <QIcon>
#include <QStyle>
#include <QString>

// Bundled GNOME-Wine icons (0.3.8). Default mode never touches the system theme.
// With Preferences "Use system icon theme" on, falls back to QIcon::fromTheme / standardIcon.
namespace appicons {

// Freedesktop name → QIcon (hue-shifted to the current theme accent when using the bundle).
QIcon icon(const QString &freedesktopName);
// Map a Qt standard pixmap to our bundle (or system when the pref is on).
QIcon standard(QStyle::StandardPixmap sp);
// Re-apply toolbar/menu icons after a theme change or pref toggle. Returns elapsed ms.
int refreshAll(class QWidget *win);
// True when the current icon for `name` was loaded from the bundle (not the system).
bool isBundled(const QString &freedesktopName);
// Logical names Libreworks uses (for tests / contact sheet).
QStringList bundledNames();
// Target hue (degrees) for a Libreworks theme id.
double themeHue(const QString &themeId);
// Apply hue shift to a pixmap: coloured pixels move from wineHue toward themeHue; greys unchanged.
QImage hueShift(const QImage &src, double targetHueDeg, bool brighten);

} // namespace appicons
