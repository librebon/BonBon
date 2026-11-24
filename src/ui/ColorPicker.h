#pragma once
#include <QColor>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QRect>
#include <QVector>
#include <QFrame>
#include <QWidget>

class QToolButton;
class QColorDialog;
class QLineEdit;

// v0.3.4: the one colour picker (eyedropper) behind every colour selector in the app.
//
// ScreenColorPicker::start() samples a colour from anywhere the platform allows:
//  - Screen: X11 / Windows / macOS (and Qt's offscreen platform): every screen is grabbed when
//    picking starts, so the colour is exactly what was on screen (the canvas as displayed,
//    panels, other applications), on every monitor and at the right device pixel on high-DPI.
//  - Portal: Wayland, where clients cannot read the screen: xdg-desktop-portal's
//    Screenshot.PickColor over D-Bus (the compositor shows its own picker).
//  - AppWindows: the fallback (Wayland without the portal, or a screen grab that fails):
//    only this application's own windows, as they were drawn when picking started.
// While hovering, a small preview follows the pointer with the colour swatch, a magnified
// view and the hex value; a click picks, Esc or a right-click cancels.
class ScreenColorPicker : public QWidget {
    Q_OBJECT
public:
    enum Mode { Auto, Screen, Portal, AppWindows };
    // The mode Auto resolves to on this platform (LW_COLOR_PICKER=screen|portal|app overrides).
    static Mode autoMode();
    // Starts picking (one picker at a time: a running one is cancelled first). Owner: the
    // widget that asked (the picker closes with it).
    static ScreenColorPicker *start(QWidget *owner, Mode mode = Auto);
    static ScreenColorPicker *active();
    Mode mode() const { return m_mode; }
    // The colour shown at a global (logical) position, as captured when picking started.
    QColor sampleAt(const QPoint &globalPos) const;
    QColor current() const { return m_cur; }
    void cancel();
    void pick(const QColor &c);
    // the device pixel of `img` (device pixel ratio `dpr`) under logical point `local`, clamped
    static QColor sampleImage(const QImage &img, qreal dpr, const QPoint &local);
    // xdg-desktop-portal's PickColor reply ("color": (ddd) in 0..1); invalid when absent
    static QColor colorFromPortalResults(const QVariantMap &results);
signals:
    void hovered(const QColor &c);   // live preview while moving
    void picked(const QColor &c);
    void cancelled();
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void hideEvent(QHideEvent *e) override;
private slots:
    void portalResponse(uint response, const QVariantMap &results);
private:
    ScreenColorPicker(QWidget *owner, Mode mode);
    bool captureScreens();
    void captureAppWindows();
    bool startPortal();
    void hoverAt(const QPoint &globalPos);
    void finish();
    struct Shot { QRect geom; QImage img; qreal dpr = 1; };
    Mode m_mode;
    QVector<Shot> m_screens;                       // Screen mode: one per QScreen
    QHash<QWidget *, Shot> m_windows;              // AppWindows mode: one per top-level
    QPoint m_pos;
    QColor m_cur;
    bool m_done = false;
    QPointer<QWidget> m_owner;
};

namespace colorpick {
// The eyedropper button (the Eyedropper tool's icon), objectName "eyedropper".
QToolButton *eyedropperButton(QWidget *parent);
// The full colour dialog (Qt's own widgets, so it looks the same everywhere) with the
// eyedropper button in place of Qt's "Pick Screen Color", which cannot work on Wayland.
// Returns an invalid colour when cancelled.
QColor getColor(const QColor &initial, QWidget *parent, const QString &title = QString(), bool alpha = true);
// Adds the eyedropper to an existing (non-native) QColorDialog; picking previews live.
void addEyedropper(QColorDialog *dlg);
}

// The Fireworks-style swatch pop-up a ColorButton opens: the current / hovered colour and its
// hex value, the eyedropper, the full colour dialog ("..."), and the 216 web-safe colours with
// a row of greys. Hovering a swatch previews it; a click or Enter in the hex field picks.
class ColorPopup : public QFrame {
    Q_OBJECT
public:
    ColorPopup(const QColor &current, bool alpha, QWidget *parent);
    static QVector<QColor> swatches();   // 18 x 13: web-safe cube + greys
    QRect swatchRect(int index) const;   // in this pop-up's coordinates
    void addTransparentChoice();         // a "Transparent" button that picks QColor(0, 0, 0, 0)
    QColor workingColor() const { return m_current; }
signals:
    void hovered(const QColor &c);
    void picked(const QColor &c);
    void eyedropperRequested();
    void dialogRequested();
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *e) override;
private:
    int swatchAt(const QPoint &p) const;
    void showColor(const QColor &c);
    void setAlphaPercent(int pct, bool preview);
    QColor m_current, m_shown;
    bool m_alpha;
    bool m_committed = false;
    bool m_alphaEdited = false;
    int m_hover = -1;
    QLineEdit *m_hex = nullptr;
    class QSlider *m_alphaSlider = nullptr;
    class QLabel *m_alphaLabel = nullptr, *m_alphaValue = nullptr;
    QRect m_preview, m_grid;
};
