#include "AppIcons.h"
#include "AppState.h"
#include "Theme.h"
#include <QApplication>
#include <QWidget>
#include <QAction>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPaintDevice>
#include <QPixmap>
#include <QStyle>
#include <QtMath>
#include <QElapsedTimer>
#include <cmath>



// Pull the icons.qrc object out of the static library (otherwise the linker drops it).
extern int qInitResources_icons();
namespace {
const int kIconsResource = qInitResources_icons();
}

namespace appicons {

namespace {

struct MapEntry { QStyle::StandardPixmap sp; const char *name; };
const MapEntry kStdMap[] = {
    {QStyle::SP_FileIcon, "document-new"},
    {QStyle::SP_FileDialogNewFolder, "folder-new"},
    {QStyle::SP_DialogOpenButton, "document-open"},
    {QStyle::SP_DirOpenIcon, "document-open"},
    {QStyle::SP_DialogSaveButton, "document-save"},
    {QStyle::SP_ArrowBack, "edit-undo"},
    {QStyle::SP_ArrowForward, "edit-redo"},
    {QStyle::SP_ArrowLeft, "go-previous"},
    {QStyle::SP_ArrowRight, "go-next"},
    {QStyle::SP_ArrowUp, "go-up"},
    {QStyle::SP_ArrowDown, "go-down"},
    {QStyle::SP_MediaSeekBackward, "go-previous"},
    {QStyle::SP_MediaSeekForward, "go-next"},
    {QStyle::SP_BrowserReload, "view-refresh"},
    {QStyle::SP_TitleBarCloseButton, "window-close"},
    {QStyle::SP_DialogCloseButton, "window-close"},
    {QStyle::SP_DialogOkButton, "dialog-ok"},
    {QStyle::SP_DialogCancelButton, "dialog-cancel"},
    {QStyle::SP_DialogApplyButton, "dialog-ok"},
    {QStyle::SP_DialogHelpButton, "help-contents"},
    {QStyle::SP_FileDialogDetailedView, "view-list-details"},
    {QStyle::SP_FileDialogListView, "view-list-icons"},
    {QStyle::SP_TrashIcon, "user-trash"},
    {QStyle::SP_DirIcon, "folder"},
    {QStyle::SP_DirHomeIcon, "user-home"},
    {QStyle::SP_ComputerIcon, "computer"},
    {QStyle::SP_DesktopIcon, "user-desktop"},
    {QStyle::SP_DialogResetButton, "edit-clear"},
    {QStyle::SP_LineEditClearButton, "edit-clear"},
    {QStyle::SP_TitleBarMaxButton, "view-fullscreen"},
    {QStyle::SP_TitleBarNormalButton, "view-restore"},
};

// Fallback chain when a primary name is missing from the bundle
QStringList aliases(const QString &name) {
    static const QHash<QString, QStringList> a = {
        {QStringLiteral("edit-undo"), {QStringLiteral("go-previous"), QStringLiteral("gtk-go-back-ltr")}},
        {QStringLiteral("edit-redo"), {QStringLiteral("go-next"), QStringLiteral("gtk-go-forward-ltr")}},
        {QStringLiteral("document-open"), {QStringLiteral("gtk-open"), QStringLiteral("fileopen")}},
        {QStringLiteral("view-fullscreen"), {QStringLiteral("gtk-fullscreen")}},
        {QStringLiteral("view-restore"), {QStringLiteral("gtk-leave-fullscreen")}},
        {QStringLiteral("edit-select-all"), {QStringLiteral("gtk-select-all")}},
        {QStringLiteral("go-up"), {QStringLiteral("gtk-go-up")}},
        {QStringLiteral("go-down"), {QStringLiteral("gtk-go-down")}},
    };
    return a.value(name);
}

QStringList sizesFor(int px) {
    // Prefer exact, then nearest larger, then scalable
    QStringList out;
    const int cand[] = {16, 22, 24, 32, 48};
    int best = 16;
    for (int c : cand) if (c >= px) { best = c; break; }
    else best = c;
    out << QStringLiteral("%1x%1").arg(best);
    for (int c : cand) {
        const QString s = QStringLiteral("%1x%1").arg(c);
        if (!out.contains(s)) out << s;
    }
    out << QStringLiteral("scalable");
    return out;
}

QImage loadBundledImage(const QString &name, int px) {
    QStringList tryNames = QStringList{name} << aliases(name);
    for (const QString &n : tryNames) {
        for (const QString &sz : sizesFor(px)) {
            const QString png = QStringLiteral(":/icons/gnome-wine/%1/actions/%2.png").arg(sz, n);
            if (QFile::exists(png)) {
                QImage im(png);
                if (!im.isNull()) return im;
            }
            const QString svg = QStringLiteral(":/icons/gnome-wine/%1/actions/%2.svg").arg(sz, n);
            if (QFile::exists(svg)) {
                // PNG preferred; SVG via QIcon if needed
                QIcon ic(svg);
                QPixmap pm = ic.pixmap(QSize(px, px));
                if (!pm.isNull()) return pm.toImage();
            }
        }
    }
    return {};
}

} // namespace

static double hslHue(const QColor &c) {
    return c.hslHueF() < 0 ? 0.0 : c.hslHueF() * 360.0;
}

double themeHue(const QString &themeId) {
    // Use the theme's accent colour hue. Custom / user themes fall through to theme::color.
    Q_UNUSED(themeId);
    return hslHue(theme::color("accent"));
}

QImage hueShift(const QImage &srcIn, double targetHueDeg, bool brighten) {
    QImage src = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out = src.copy();
    const double target = std::fmod(targetHueDeg + 360.0, 360.0);
    for (int y = 0; y < out.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            QColor c = QColor::fromRgba(line[x]);
            if (c.alpha() < 8) continue;
            // Un-premultiply for HSL
            if (c.alpha() < 255 && c.alpha() > 0) {
                const double a = c.alphaF();
                c.setRgbF(std::min(1.0, c.redF() / a), std::min(1.0, c.greenF() / a), std::min(1.0, c.blueF() / a), a);
            }
            const double sat = c.hslSaturationF();
            // Neutral greys stay neutral (low saturation)
            if (sat < 0.12) continue;
            const double lit = c.lightnessF();
            double L = lit;
            if (brighten) L = std::min(0.92, L * 1.08 + 0.04);
            c.setHslF(target / 360.0, sat, L, c.alphaF());
            // Re-premultiply
            const double a = c.alphaF();
            line[x] = qRgba(int(c.redF() * a * 255 + 0.5), int(c.greenF() * a * 255 + 0.5),
                            int(c.blueF() * a * 255 + 0.5), c.alpha());
        }
    }
    return out;
}

namespace {
class WineIconEngine : public QIconEngine {
public:
    explicit WineIconEngine(QString name) : m_name(std::move(name)) {}
    QString key() const override { return QStringLiteral("lw-wine:") + m_name; }
    QIconEngine *clone() const override { return new WineIconEngine(m_name); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        Q_UNUSED(state);
        const int px = qMax(size.width(), size.height());
        const QString themeId = appState().prefs.theme;
        const bool sys = appState().prefs.useSystemIcons;
        // Include ink colours so a Custom-from-colour seed change (same theme id)
        // invalidates the pixmap. Built-in themes keep a stable key+image.
        const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
            .arg(m_name, themeId)
            .arg(theme::color("accent").rgba(), 0, 16)
            .arg(theme::color("window").rgba(), 0, 16)
            .arg(px).arg(int(mode)).arg(sys ? 1 : 0);
        if (m_cacheKey == cacheKey && !m_cached.isNull()) return m_cached;

        QPixmap pm;
        if (sys) {
            pm = QIcon::fromTheme(m_name).pixmap(size, scale, mode);
            if (pm.isNull()) {
                // last resort when the system theme lacks the name
                QImage im = loadBundledImage(m_name, px);
                if (!im.isNull()) pm = QPixmap::fromImage(im.scaled(size * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        } else {
            QImage im = loadBundledImage(m_name, int(px * scale));
            if (im.isNull()) {
                m_cacheKey = cacheKey;
                m_cached = {};
                return {};
            }
            const double hue = themeHue(themeId);
            const bool dark = theme::color("window").lightnessF() < 0.45;
            im = hueShift(im, hue, dark);
            if (im.size() != size * scale)
                im = im.scaled(size * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pm = QPixmap::fromImage(im);
            if (mode == QIcon::Disabled) {
                QPixmap d(pm.size());
                d.fill(Qt::transparent);
                QPainter p(&d);
                p.setOpacity(0.4);
                p.drawPixmap(0, 0, pm);
                p.end();
                pm = d;
            }
        }
        m_cacheKey = cacheKey;
        m_cached = pm;
        return pm;
    }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal dpr = (p->device()) ? p->device()->devicePixelRatioF() : 1.0;
        p->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }
    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override {
        return {{16, 16}, {22, 22}, {24, 24}, {32, 32}, {48, 48}};
    }
private:
    QString m_name;
    QString m_cacheKey;
    QPixmap m_cached;
};
} // namespace

QIcon icon(const QString &freedesktopName) {
    return QIcon(new WineIconEngine(freedesktopName));
}

QIcon standard(QStyle::StandardPixmap sp) {
    if (appState().prefs.useSystemIcons)
        return QApplication::style()->standardIcon(sp);
    for (const MapEntry &e : kStdMap)
        if (e.sp == sp) return icon(QString::fromLatin1(e.name));
    // Unknown SP_*: try style but never leave the UI without an icon in default mode —
    // return a bundled go-next as a last-ditch coloured mark rather than Qt grey.
    QIcon sys = QApplication::style()->standardIcon(sp);
    if (!appState().prefs.useSystemIcons) {
        // Prefer bundle arrow so we never show grey Qt fallbacks in default mode
        QIcon bundled = icon(QStringLiteral("go-next"));
        if (!bundled.isNull() && !bundled.availableSizes().isEmpty()) return bundled;
    }
    return sys;
}

bool isBundled(const QString &freedesktopName) {
    if (appState().prefs.useSystemIcons) return false;
    return !loadBundledImage(freedesktopName, 16).isNull();
}

QStringList bundledNames() {
    return {
        QStringLiteral("document-new"), QStringLiteral("document-open"), QStringLiteral("document-save"),
        QStringLiteral("edit-undo"), QStringLiteral("edit-redo"), QStringLiteral("edit-select-all"),
        QStringLiteral("go-up"), QStringLiteral("go-down"), QStringLiteral("go-previous"), QStringLiteral("go-next"),
        QStringLiteral("go-first"), QStringLiteral("go-last"), QStringLiteral("go-top"), QStringLiteral("go-bottom"),
        QStringLiteral("folder-new"), QStringLiteral("view-fullscreen"), QStringLiteral("view-restore"),
        QStringLiteral("zoom-in"), QStringLiteral("zoom-out"),
    };
}

int refreshAll(QWidget *win) {
    QElapsedTimer t; t.start();
    if (!win) return 0;
    auto set = [&](const char *id, QStyle::StandardPixmap sp) {
        if (QAction *a = win->findChild<QAction *>(QString::fromLatin1(id))) a->setIcon(standard(sp));
        // also match by objectName among all actions
        for (QAction *a : win->findChildren<QAction *>())
            if (a->objectName() == QLatin1String(id)) { a->setIcon(standard(sp)); break; }
    };
    set("file.new", QStyle::SP_FileIcon);
    set("file.open", QStyle::SP_DialogOpenButton);
    set("file.save", QStyle::SP_DialogSaveButton);
    set("edit.undo", QStyle::SP_ArrowBack);
    set("edit.redo", QStyle::SP_ArrowForward);
    set("file.import", QStyle::SP_ArrowDown);
    set("file.export", QStyle::SP_ArrowUp);
    // Notify widgets that use standard icons directly (ZoomField recreates on style change)
    QEvent ev(QEvent::StyleChange);
    for (QWidget *w : win->findChildren<QWidget *>())
        QApplication::sendEvent(w, &ev);
    return int(t.elapsed());
}

} // namespace appicons
