#include "Theme.h"
#include "Brand.h"
#include "AppState.h"
#include "WidgetEnable.h"
#include "ComboFit.h"
#include <QApplication>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QWidget>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QProxyStyle>
#include <QPointer>
#include <QAbstractItemView>
#include <QStyleOptionViewItem>
#include <QStyleOption>
#include <QStyleOptionToolButton>
#include <QStyleOptionTitleBar>
#include <QTabBar>
#include <QTabWidget>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QMenuBar>
#include <QMenu>
#include <QDockWidget>
#include <QAbstractButton>
#include <QPushButton>
#include <QToolButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QTreeWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QStatusBar>
#include <QScrollBar>
#include <QSlider>
#include <QLabel>
#include <QMainWindow>
#include <QDialog>
#include <QToolTip>
#include <QTimer>
#include <QTextDocument>
#include <QFontMetrics>
#include <QEvent>
#include <QAbstractSpinBox>
#include <cmath>
#include <utility>
#include <initializer_list>

namespace theme {

// Stylesheet template: @role@ tokens are replaced by the theme's colours, @ARROWS@ by the
// directory of the theme's arrow images.
static const char *kStyle = R"(
QMainWindow, QDialog { background: palette(window); }
QWidget { font-size: 9pt; color: palette(window-text); }
QWidget:disabled { color: palette(placeholder-text); }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled, QPlainTextEdit:disabled { background: palette(base); color: palette(placeholder-text); }
QPushButton:disabled { color: palette(placeholder-text); background: palette(window); border-color: palette(mid); }
QMainWindow::separator { background: palette(alternate-base); width: 4px; height: 4px; }
QMainWindow::separator:hover { background: palette(dark); }
QMenuBar { background: palette(window); border-bottom: 1px solid palette(mid); }
QMenuBar::item { padding: 3px 8px; background: transparent; }
QMenuBar::item:selected { background: palette(highlight); color: palette(highlighted-text); }
QMenu { background: palette(window); border: 1px solid palette(link-visited); }
QMenu::item { padding: 3px 26px 3px 22px; }
QMenu::item:selected { background: palette(highlight); color: palette(highlighted-text); }
QMenu::item:disabled { color: palette(placeholder-text); }
QMenu::separator { height: 1px; background: palette(mid); margin: 3px 4px; }
#PITitle { font-weight: bold; font-size: 10pt; }
QTabBar { outline: none; }
QAbstractItemView { outline: none; }
QTabBar::tab {
  background: palette(button); border: 1px solid palette(dark); border-bottom: none; padding: 3px 12px; margin-right: 1px;
  border-top-left-radius: 3px; border-top-right-radius: 3px;
}
QTabBar::tab:selected { background: palette(base); border-top: 2px solid palette(link); padding-top: 2px; }
QTabBar::tab:hover:!selected { background: palette(light); }
QTabWidget::pane { border: 1px solid palette(dark); }
QTabBar#ViewTabs::tab { padding: 2px 10px; }
QTabBar#ViewTabs::tab:selected { padding-top: 1px; }
QPushButton {
  border: 1px solid palette(link-visited); border-radius: 2px; padding: 3px 10px; min-height: 16px;
  background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 palette(button), stop:1 palette(light));
}
QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 palette(alternate-base), stop:1 palette(window)); }
QPushButton:pressed { background: palette(mid); }
QPushButton:default { border: 1px solid palette(highlight); }
QToolButton { border: 1px solid transparent; padding: 2px; }
QToolButton:hover { border: 1px solid palette(dark); background: palette(base); }
QToolButton:checked { border: 1px solid palette(shadow); background: palette(midlight); }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
  background: palette(base); border: 1px solid palette(dark); padding: 1px 3px; selection-background-color: palette(highlight);
}
QComboBox::drop-down {
  subcontrol-origin: padding; subcontrol-position: top right; width: 14px;
  border-left: 1px solid palette(mid); background: palette(midlight);
}
QComboBox::drop-down:hover { background: palette(alternate-base); }
QComboBox::drop-down:on { background: palette(button); }
QComboBox::drop-down:disabled { background: palette(base); }
QComboBox::down-arrow { image: url(@ARROWS@/down.png); width: 7px; height: 4px; }
QComboBox::down-arrow:hover { image: url(@ARROWS@/down-hover.png); }
QComboBox::down-arrow:on { image: url(@ARROWS@/down-open.png); top: 1px; }
QComboBox::down-arrow:disabled { image: url(@ARROWS@/down-disabled.png); }
QSpinBox, QDoubleSpinBox { padding-right: 15px; }
PopupSliderField, PopupSliderDoubleField, QAbstractSpinBox[buttonSymbols="NoButtons"] { padding-right: 3px; }
QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button {
  subcontrol-origin: padding; width: 14px; border-left: 1px solid palette(mid); background: palette(midlight);
}
QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-position: top right; }
QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-position: bottom right; }
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover { background: palette(alternate-base); }
QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed, QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed { background: palette(button); }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@ARROWS@/up.png); width: 7px; height: 4px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@ARROWS@/down.png); width: 7px; height: 4px; }
QSpinBox::up-arrow:disabled, QDoubleSpinBox::up-arrow:disabled, QSpinBox::up-arrow:off, QDoubleSpinBox::up-arrow:off { image: url(@ARROWS@/up-disabled.png); }
QSpinBox::down-arrow:disabled, QDoubleSpinBox::down-arrow:disabled, QSpinBox::down-arrow:off, QDoubleSpinBox::down-arrow:off { image: url(@ARROWS@/down-disabled.png); }
QListWidget, QTreeWidget { background: palette(base); border: 1px solid palette(dark); alternate-background-color: palette(alternate-base); }
QListWidget::item:selected, QTreeWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }
QHeaderView::section { background: palette(light); border: 1px solid palette(mid); padding: 2px; }
QGroupBox { border: 1px solid palette(mid); margin-top: 8px; padding-top: 4px; }
QGroupBox::title { subcontrol-origin: margin; left: 6px; padding: 0 3px; color: palette(window-text); }
QStatusBar { background: palette(window); border-top: 1px solid palette(mid); }
QScrollBar:vertical {
  width: 14px; background: palette(midlight);
  margin: 16px 0 16px 0;
}
QScrollBar:horizontal {
  height: 14px; background: palette(midlight);
  margin: 0 16px 0 16px;
}
QScrollBar::handle {
  background: palette(button); border: 1px solid palette(dark);
  min-height: 20px; min-width: 20px;
}
QScrollBar::handle:hover { background: palette(light); }
QScrollBar::add-line:vertical {
  height: 16px; width: 14px; subcontrol-position: bottom; subcontrol-origin: margin;
  background: palette(midlight); border: none;
}
QScrollBar::sub-line:vertical {
  height: 16px; width: 14px; subcontrol-position: top; subcontrol-origin: margin;
  background: palette(midlight); border: none;
}
QScrollBar::add-line:horizontal {
  width: 16px; height: 14px; subcontrol-position: right; subcontrol-origin: margin;
  background: palette(midlight); border: none;
}
QScrollBar::sub-line:horizontal {
  width: 16px; height: 14px; subcontrol-position: left; subcontrol-origin: margin;
  background: palette(midlight); border: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
QScrollBar::up-arrow:vertical, QScrollBar::down-arrow:vertical {
  width: 7px; height: 4px; background: palette(dark);
}
QScrollBar::left-arrow:horizontal, QScrollBar::right-arrow:horizontal {
  width: 4px; height: 7px; background: palette(dark);
}
QSlider::groove:horizontal { height: 4px; background: palette(mid); }
QSlider::handle:horizontal { width: 9px; margin: -6px 0; background: palette(light); border: 1px solid palette(shadow); }
#RulerCorner { background: palette(window); }
#EmptyHint { color: palette(window-text); background: palette(window); font-size: 14px; }
#StubNote { color: palette(window-text); font-style: italic; }
QLabel[lwSmall="true"] { color: palette(window-text); }
QLabel[lwNote="true"] { color: palette(window-text); padding-top: 3px; }
QLabel[lwSmall="true"]:disabled, QLabel[lwNote="true"]:disabled, #StubNote:disabled { color: palette(placeholder-text); }
#statusZoom:disabled { background: palette(midlight); color: palette(placeholder-text); }
)";

namespace {
struct Base { const char *id, *name; bool dark, hc; QRgb window, text, base, accent, pasteboard; };
const Base kBases[] = {
    {"classic", "Macromedia Classic", false, false, 0xD4D2CD, 0x1E1E1E, 0xFFFFFF, 0x2A5DA8, 0x9C9993},
    {"light", "Light", false, false, 0xECECEC, 0x1A1A1A, 0xFFFFFF, 0x2F6FD6, 0xB8B8B8},
    {"dark", "Dark", true, false, 0x3A3A3A, 0xE6E6E6, 0x262626, 0x4A90E2, 0x2B2B2B},
    {"midnight", "Midnight", true, false, 0x1B2030, 0xDCE3F0, 0x121624, 0x5B8CFF, 0x0E111B},
    {"graphite", "Graphite", true, false, 0x424448, 0xF0F0F0, 0x303235, 0x8AA4C8, 0x3A3B3E},
    {"studio8", "Studio 8", false, false, 0xE4E8EE, 0x1C2433, 0xFFFFFF, 0x3D7CC9, 0xA7AFB9},
    {"solarized-dark", "Solarized Dark", true, false, 0x073642, 0xEEE8D5, 0x002B36, 0x268BD2, 0x00212B},
    {"solarized-light", "Solarized Light", false, false, 0xEEE8D5, 0x073642, 0xFDF6E3, 0x268BD2, 0xD3CBB7},
    {"nord", "Nord", true, false, 0x3B4252, 0xECEFF4, 0x2E3440, 0x88C0D0, 0x272C36},
    {"synthwave", "Synthwave", true, false, 0x2A2139, 0xF4EEFF, 0x1E1730, 0xFF7EDB, 0x1A1325},
    {"pink", "Pink", false, false, 0xF4E4EB, 0x2B1720, 0xFFFBFD, 0xC2185B, 0xCDB0BD},
    {"pink-night", "Pink Night", true, false, 0x33222B, 0xF8E7EF, 0x261820, 0xFF8AB8, 0x1F141A},
    {"high-contrast", "High Contrast", true, true, 0x000000, 0xFFFFFF, 0x000000, 0xFFD400, 0x202020},
    {"bonbon", "Bonbon", false, false, 0xFED2E1, 0x221016, 0xFFFDFD, 0xC23B7A, 0xBB919F},
};

const Base *findBase(const QString &id) {
    for (const Base &b : kBases) if (id == QLatin1String(b.id)) return &b;
    return nullptr;
}

QColor mix(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

double luminance(const QColor &c) {
    auto ch = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
}

// Text colour for a background, and the background nudged until it reaches 4.5:1 if needed.
void accentPair(QColor &accent, QColor &text) {
    for (int i = 0; i < 40; ++i) {
        const double w = contrast(Qt::white, accent), k = contrast(Qt::black, accent);
        text = w >= k ? QColor(Qt::white) : QColor(Qt::black);
        if (std::max(w, k) >= 4.5) return;
        accent = w >= k ? accent.darker(106) : accent.lighter(106);
    }
}

// Disabled text: a mix of text toward the window colour, dim enough to read as unavailable but
// still legible (contrast about a third of the normal text's, at least 2.6:1 against the window).
QColor disabledText(const QColor &text, const QColor &window) {
    const double target = std::clamp(contrast(text, window) / 3.0, 2.6, 4.5);
    double lo = 0, hi = 1;   // t: 0 text .. 1 window
    for (int i = 0; i < 30; ++i) {
        const double t = (lo + hi) / 2;
        if (contrast(mix(text, window, t), window) >= target) lo = t; else hi = t;
    }
    return mix(text, window, lo);
}

QMap<QString, QColor> classicRoles() {
    QMap<QString, QColor> r;
    const std::pair<const char *, QRgb> v[] = {
        {"text", 0x1E1E1E}, {"accent", 0x2A5DA8}, {"tabAccent", 0x316AC5}, {"secText", 0x3B3A36}, {"chkEdge", 0x5E5A52},
        {"sliderEdge", 0x6F6B63}, {"frame", 0x7F7B73}, {"inputEdge", 0x8D897F}, {"hovEdge", 0x8E8A82}, {"edge", 0x8F8B83},
        {"sepHover", 0x9C988F}, {"secLo", 0x9D998F}, {"bodyEdge", 0xA6A29A}, {"line", 0xA9A59D}, {"chkBg", 0xB5B1A9},
        {"sep", 0xB9B6AF}, {"secBg", 0xBEBAB2}, {"tbChkBg", 0xC2BEB6}, {"menuSep", 0xC3C0B9}, {"pressed", 0xC7C3BB},
        {"hdrBot", 0xC9C5BD}, {"tab", 0xCBC8C1}, {"phHovBot", 0xD3CFC8}, {"window", 0xD4D2CD}, {"tools", 0xD8D5CF},
        {"secHi", 0xDAD7D1}, {"pi", 0xE1DFDA}, {"bar", 0xE3E1DC}, {"body", 0xE6E4DF}, {"hovBg", 0xECEAE5},
        {"dropDis", 0xECEAE6}, {"tabSel", 0xF0EEEA}, {"hdrTop", 0xF1EFEB}, {"phTop", 0xF3F1ED}, {"altBase", 0xF4F3F0},
        {"menu", 0xF5F4F1}, {"phHovTop", 0xFAF9F6}, {"btnTop", 0xFBFAF8}, {"tipBg", 0xFFFFE1}, {"btnHovTop", 0xFFFFFF},
        {"accentText", 0xFFFFFF}, {"input", 0xFFFFFF}, {"tipText", 0x000000}, {"button", 0xE0DED9}, {"ruler", 0xECEAE4},
        {"rulerInk", 0x6E6E6E}, {"small", 0x333333}, {"emptyInk", 0x2B2925}, {"arrow", 0x1A1A1A}, {"light", 0xFAF9F7}, {"currentRow", 0xD9E3F0},
    };
    for (const auto &[k, c] : v) r.insert(QString::fromLatin1(k), QColor(c));
    return r;
}


QMap<QString, QColor> generatedRoles(const Base &b) {
    const QColor W(b.window), T(b.text), B(b.base), A(b.accent);
    // raised (lighter) and recessed (darker) surfaces, and edges that lean toward the text colour
    auto L = [&](double t) { return b.dark ? mix(W, T, t * 0.22) : mix(W, Qt::white, t); };
    auto D = [&](double t) { return b.dark ? mix(W, Qt::black, t * 0.45) : mix(W, T, t * 0.3); };
    auto E = [&](double t) { return b.hc ? T : mix(W, T, t); };
    QMap<QString, QColor> r;
    auto set = [&](const char *k, const QColor &c) { r.insert(QString::fromLatin1(k), c); };
    set("window", W); set("text", T); set("input", B);
    set("sep", D(0.4)); set("sepHover", E(0.3)); set("bar", L(0.3)); set("line", E(0.3));
    set("menu", b.dark ? mix(W, B, 0.5) : L(0.6)); set("frame", E(0.45)); set("menuSep", E(0.2));
    set("hdrTop", L(0.6)); set("hdrBot", D(0.35)); set("edge", E(0.38));
    set("phTop", L(0.65)); set("pressed", D(0.45)); set("phHovTop", L(0.85)); set("phHovBot", D(0.15));
    set("body", L(0.35)); set("bodyEdge", E(0.32));
    set("secText", b.hc ? T : mix(T, W, 0.18)); set("secBg", D(0.5)); set("secHi", L(0.3)); set("secLo", E(0.35));
    set("tools", L(0.05)); set("hovEdge", b.hc ? A : E(0.4)); set("hovBg", L(0.5));
    set("chkEdge", b.hc ? A : E(0.6)); set("chkBg", D(0.6));
    set("pi", L(0.25)); set("tab", D(0.3)); set("tabSel", L(0.6)); set("tabAccent", A);
    set("btnTop", L(0.8)); set("btnHovTop", L(1.0)); set("button", L(0.3));
    set("tbChkBg", D(0.45)); set("inputEdge", E(0.42)); set("dropDis", b.dark ? mix(W, B, 0.5) : L(0.4));
    set("altBase", mix(B, W, 0.35));
    set("tipBg", b.dark ? L(0.6) : QColor(0xFF, 0xFF, 0xE1)); set("tipText", b.dark ? T : QColor(Qt::black));
    set("sliderEdge", E(0.55)); set("ruler", L(0.45)); set("rulerInk", b.hc ? T : mix(W, T, 0.6));
    set("small", T); set("emptyInk", mix(QColor(b.pasteboard), T, 0.9)); set("arrow", T); set("light", L(1.0)); set("currentRow", mix(B, A, b.dark ? 0.3 : 0.15));
    return r;
}

// ---- OKLab / OKLCH (Bjorn Ottosson) ----
double toLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double toGamma(double v) { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
constexpr double kPi = 3.14159265358979323846;

void oklchToLinear(double L, double C, double h, double &r, double &g, double &b) {
    const double a = C * std::cos(h * kPi / 180), bb = C * std::sin(h * kPi / 180);
    const double l_ = L + 0.3963377774 * a + 0.2158037573 * bb;
    const double m_ = L - 0.1055613458 * a - 0.0638541728 * bb;
    const double s_ = L - 0.0894841775 * a - 1.2914855480 * bb;
    const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

// a colour at the same hue / chroma with another OKLCH lightness

QColor oklabMix(const QColor &x, const QColor &y, double t) {
    const Oklch a = toOklch(x), b = toOklch(y);
    const double ax = a.C * std::cos(a.h * kPi / 180), ay = a.C * std::sin(a.h * kPi / 180);
    const double bx = b.C * std::cos(b.h * kPi / 180), by = b.C * std::sin(b.h * kPi / 180);
    const double mx = ax + (bx - ax) * t, my = ay + (by - ay) * t;
    return fromOklch(a.L + (b.L - a.L) * t, std::hypot(mx, my), std::atan2(my, mx) * 180 / kPi);
}

// Push fg's lightness (hue and chroma kept) away from the backgrounds until it reaches `min`
// contrast against all of them; the smaller lightness change wins. Unreachable: the best extreme.
QColor ensure(const QColor &fg, const QVector<QColor> &bgs, double min) {
    auto worst = [&](const QColor &c) { double w = 99; for (const QColor &b : bgs) w = std::min(w, contrast(c, b)); return w; };
    if (worst(fg) >= min) return fg;
    const Oklch o = toOklch(fg);
    QColor best; double bestDelta = 9;
    for (double target : {0.0, 1.0})
        for (int i = 1; i <= 200; ++i) {
            const double L = o.L + (target - o.L) * i / 200.0;
            const QColor c = fromOklch(L, o.C, o.h);
            if (worst(c) >= min) {
                if (std::abs(L - o.L) < bestDelta) { bestDelta = std::abs(L - o.L); best = c; }
                break;
            }
        }
    if (best.isValid()) return best;
    // neither direction is enough even at the extreme: pure black / white, whichever is better
    return worst(Qt::black) >= worst(Qt::white) ? QColor(Qt::black) : QColor(Qt::white);
}

QMap<QString, QColor> seedRoles(const Spec &sp, bool dark, QColor *accentOut, QColor *pasteboardOut) {
    const QColor seed = sp.seed.isValid() ? sp.seed.toRgb() : QColor(0xC2, 0x3B, 0x7A);
    const Oklch s = toOklch(seed);
    const double v = std::clamp(sp.vibrancy, 0, 100) / 100.0;
    const bool chromatic = s.C >= 0.02;
    const double hue = s.h;
    const double nc = chromatic ? std::min(s.C, 0.2) * v * 0.3 : 0.0;   // neutral palette chroma (<= 0.06)
    auto N = [&](double L) { return fromOklch(L, nc, hue); };            // surfaces
    auto NV = [&](double L) { return fromOklch(L, nc * 1.6, hue); };     // edges and borders
    auto T = [&](double L) { return fromOklch(L, nc * 0.6, hue); };      // inks
    // role -> (palette, light tone, dark tone)
    struct Tone { const char *role; char pal; double light, dark; };
    static const Tone tones[] = {
        {"window", 'N', 0.905, 0.300}, {"input", 'N', 0.995, 0.225}, {"altBase", 'N', 0.955, 0.262},
        {"bar", 'N', 0.935, 0.335}, {"menu", 'N', 0.965, 0.262}, {"body", 'N', 0.940, 0.338},
        {"pi", 'N', 0.928, 0.322}, {"tools", 'N', 0.915, 0.305}, {"tab", 'N', 0.865, 0.262},
        {"tabSel", 'N', 0.968, 0.372}, {"hdrTop", 'N', 0.968, 0.372}, {"hdrBot", 'N', 0.855, 0.252},
        {"phTop", 'N', 0.975, 0.382}, {"pressed", 'N', 0.840, 0.242}, {"phHovTop", 'N', 0.988, 0.420},
        {"phHovBot", 'N', 0.890, 0.288}, {"btnTop", 'N', 0.985, 0.400}, {"btnHovTop", 'N', 1.000, 0.450},
        {"secHi", 'N', 0.930, 0.342}, {"secBg", 'N', 0.830, 0.242}, {"hovBg", 'N', 0.955, 0.372},
        {"chkBg", 'N', 0.800, 0.222}, {"tbChkBg", 'N', 0.830, 0.242}, {"dropDis", 'N', 0.948, 0.262},
        {"button", 'N', 0.928, 0.342}, {"light", 'N', 0.990, 0.450}, {"ruler", 'N', 0.950, 0.342},
        {"sep", 'N', 0.830, 0.222},
        {"menuSep", 'V', 0.800, 0.420}, {"line", 'V', 0.740, 0.450}, {"edge", 'V', 0.660, 0.500},
        {"bodyEdge", 'V', 0.720, 0.460}, {"secLo", 'V', 0.680, 0.450}, {"sepHover", 'V', 0.680, 0.550},
        {"frame", 'V', 0.580, 0.600}, {"inputEdge", 'V', 0.600, 0.580}, {"hovEdge", 'V', 0.600, 0.620},
        {"chkEdge", 'V', 0.480, 0.700}, {"sliderEdge", 'V', 0.520, 0.640},
        {"text", 'T', 0.200, 0.940}, {"secText", 'T', 0.300, 0.840}, {"small", 'T', 0.240, 0.920},
        {"rulerInk", 'T', 0.420, 0.760}, {"arrow", 'T', 0.200, 0.940}, {"emptyInk", 'T', 0.220, 0.880},
        {"tipText", 'T', 0.180, 0.950},
    };
    QMap<QString, QColor> r;
    for (const Tone &t : tones) {
        const double L = dark ? t.dark : t.light;
        r.insert(QString::fromLatin1(t.role), t.pal == 'N' ? N(L) : t.pal == 'V' ? NV(L) : T(L));
    }
    auto c = [&](const char *k) -> QColor & { return r[QString::fromLatin1(k)]; };
    const QColor pasteboard = N(dark ? 0.205 : 0.700);
    // tooltip: a light tint of the seed (classic's pale yellow, re-hued), or a raised neutral in dark
    c("tipBg") = dark ? N(0.420) : fromOklch(0.965, chromatic ? std::min(s.C, 0.035) : 0.0, hue);
    // accent: the seed itself when usable, pushed to >= 3:1 against the window (it is also the tab
    // accent border and the default-button frame); its text colour comes later (accentPair, 4.5:1)
    QColor accent = ensure(seed, {c("window"), c("body")}, 3.0);
    c("tabAccent") = accent;
    c("currentRow") = oklabMix(c("input"), accent, dark ? 0.30 : 0.15);
    // enforce contrast: text, inks, icons, borders
    c("text") = ensure(c("text"), {c("window"), c("input")}, 7.0);
    QVector<QColor> textBgs;
    for (const char *bg : {"bar", "menu", "body", "pi", "tools", "tab", "tabSel", "hdrTop", "hdrBot", "phTop", "phHovTop", "phHovBot",
                           "btnTop", "btnHovTop", "secHi", "pressed", "hovBg", "chkBg", "tbChkBg", "altBase", "button", "currentRow",
                           "dropDis", "secBg", "light", "ruler"})
        textBgs << c(bg);
    c("text") = ensure(c("text"), textBgs, 4.5);
    c("arrow") = ensure(c("arrow"), textBgs + QVector<QColor>{c("window"), c("input")}, 4.5);
    c("small") = ensure(c("small"), {c("pi"), c("window"), c("body")}, 4.5);
    c("secText") = ensure(c("secText"), {c("secBg"), c("window"), c("body"), c("secHi")}, 4.5);
    c("tipText") = ensure(c("tipText"), {c("tipBg")}, 4.5);
    c("emptyInk") = ensure(c("emptyInk"), {pasteboard}, 4.5);
    c("rulerInk") = ensure(c("rulerInk"), {c("ruler")}, 3.0);
    for (const char *k : {"frame", "inputEdge"}) c(k) = ensure(c(k), {c("window"), c("body"), c("input"), c("menu")}, 3.0);
    for (const char *k : {"hovEdge", "chkEdge"}) c(k) = ensure(c(k), {c("tools"), c("hovBg"), c("chkBg"), c("tbChkBg"), c("tabSel")}, 3.0);
    c("sliderEdge") = ensure(c("sliderEdge"), {c("hdrTop"), c("window"), c("body")}, 3.0);
    if (accentOut) *accentOut = accent;
    if (pasteboardOut) *pasteboardOut = pasteboard;
    return r;
}

// The registry of generated themes (see setGenerated)
Spec g_custom;
QVector<Spec> g_user;

bool parseMode(const QString &m, int *out) {
    const QString l = m.trimmed().toLower();
    if (l == QLatin1String("auto") || l == QLatin1String("0")) *out = 0;
    else if (l == QLatin1String("light") || l == QLatin1String("1")) *out = 1;
    else if (l == QLatin1String("dark") || l == QLatin1String("2")) *out = 2;
    else return false;
    return true;
}
const char *modeName(int m) { return m == 1 ? "light" : m == 2 ? "dark" : "auto"; }

struct Source { QMap<QString, QColor> r; bool dark; QColor accent, pasteboard; };
Source source(const QString &id) {
    Spec sp;
    if (specFor(id, &sp)) {
        Source s;
        s.dark = specIsDark(sp);
        s.r = seedRoles(sp, s.dark, &s.accent, &s.pasteboard);
        return s;
    }
    const Base *b = findBase(id);
    if (!b) b = &kBases[0];
    if (QLatin1String(b->id) == QLatin1String("bonbon")) {
        // 0.3.9: Bonbon == Custom-from-colour with seed #c23b7a, Auto, vibrancy 100
        // (generator output matches packaging/themes/bonbon.json exactly: 54/54 roles).
        Spec bonbonSpec;
        bonbonSpec.name = QStringLiteral("Bonbon");
        bonbonSpec.seed = QColor(0xC2, 0x3B, 0x7A);
        bonbonSpec.mode = int(SeedMode::Auto);
        bonbonSpec.vibrancy = 100;
        Source out;
        out.dark = specIsDark(bonbonSpec);
        out.r = seedRoles(bonbonSpec, out.dark, &out.accent, &out.pasteboard);
        return out;
    }
    return {b == &kBases[0] ? classicRoles() : generatedRoles(*b), b->dark, QColor(b->accent), QColor(b->pasteboard)};
}
}

QVector<Info> list() {
    QVector<Info> out;
    for (const Base &b : kBases) out.append({QString::fromLatin1(b.id), QString::fromLatin1(b.name), b.dark});
    return out;
}

QString defaultId() {
#if LW_IS_BONBON
    return QStringLiteral("bonbon");
#else
    return QStringLiteral("classic");
#endif
}
bool exists(const QString &id) { return findBase(id) || specFor(id, nullptr); }

Oklch toOklch(const QColor &cIn) {
    const QColor c = cIn.toRgb();
    const double r = toLinear(c.redF()), g = toLinear(c.greenF()), b = toLinear(c.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    double h = std::atan2(B, A) * 180 / kPi;
    if (h < 0) h += 360;
    return {L, std::hypot(A, B), h};
}

QColor fromOklch(double L, double C, double h) {
    L = std::clamp(L, 0.0, 1.0);
    C = std::max(0.0, C);
    auto inGamut = [&](double cc, double &r, double &g, double &b) {
        oklchToLinear(L, cc, h, r, g, b);
        const double e = 1e-4;
        return r >= -e && r <= 1 + e && g >= -e && g <= 1 + e && b >= -e && b <= 1 + e;
    };
    double r, g, b;
    if (!inGamut(C, r, g, b)) {   // reduce chroma at constant lightness and hue until it fits sRGB
        double lo = 0, hi = C;
        for (int i = 0; i < 24; ++i) { const double mid = (lo + hi) / 2; double r2, g2, b2; if (inGamut(mid, r2, g2, b2)) lo = mid; else hi = mid; }
        inGamut(lo, r, g, b);
    }
    auto ch = [](double v) { return float(std::clamp(toGamma(std::clamp(v, 0.0, 1.0)), 0.0, 1.0)); };
    // round through 8-bit so the colour is exactly what the stylesheet / palette will show
    return QColor(QColor::fromRgbF(ch(r), ch(g), ch(b)).rgb());
}

bool specIsDark(const Spec &s) {
    if (s.mode == int(SeedMode::Light)) return false;
    if (s.mode == int(SeedMode::Dark)) return true;
    return toOklch(s.seed.isValid() ? s.seed : QColor(0xC2, 0x3B, 0x7A)).L < 0.55;
}

QString customId() { return QStringLiteral("custom"); }
QString userId(const QString &name) { return QStringLiteral("user:") + name; }

bool isGenerated(const QString &id) { return specFor(id, nullptr); }

void setGenerated(const Spec &custom, const QVector<Spec> &user) { g_custom = custom; g_user = user; }

void syncGenerated(const Prefs &p) {
    Spec c;
    c.seed = p.customSeed;
    c.mode = std::clamp(p.customMode, 0, 2);
    c.vibrancy = std::clamp(p.customVibrancy, 0, 100);
    setGenerated(c, p.userThemes);
}

bool specFor(const QString &idIn, Spec *out) {
    QString id = idIn;
    int forced = -1;
    if (id.endsWith(QLatin1String("@light"))) { forced = 1; id.chop(6); }
    else if (id.endsWith(QLatin1String("@dark"))) { forced = 2; id.chop(5); }
    Spec sp;
    if (id == customId()) sp = g_custom;
    else if (id.startsWith(QLatin1String("user:"))) {
        const QString name = id.mid(5);
        bool found = false;
        for (const Spec &u : g_user) if (u.name == name) { sp = u; found = true; break; }
        if (!found) return false;
    } else if (id.startsWith(QLatin1String("seed:"))) {
        const QStringList parts = id.mid(5).split(QLatin1Char(':'));
        QString hex = parts.value(0);
        if (!hex.startsWith(QLatin1Char('#'))) hex.prepend(QLatin1Char('#'));
        if (hex.size() != 7 || !QColor::isValidColorName(hex)) return false;
        sp.seed = QColor::fromString(hex);
        sp.name = hex.toUpper();
        if (parts.size() > 1 && !parseMode(parts[1], &sp.mode)) return false;
        if (parts.size() > 2) { bool ok; sp.vibrancy = std::clamp(parts[2].toInt(&ok), 0, 100); if (!ok) return false; }
    } else return false;
    if (forced >= 0) sp.mode = forced;
    if (out) *out = sp;
    return true;
}

QMap<QString, QColor> generate(const Spec &s, const QColor &accent) {
    const Spec keep = g_custom;
    g_custom = s;
    QMap<QString, QColor> r = roles(customId(), accent);
    g_custom = keep;
    return r;
}

QByteArray toJson(const Spec &s) {
    QJsonObject o;
    o.insert(QStringLiteral("format"), QStringLiteral("libreworks-theme"));
    o.insert(QStringLiteral("version"), 1);
    o.insert(QStringLiteral("name"), s.name);
    o.insert(QStringLiteral("seed"), s.seed.name());
    o.insert(QStringLiteral("mode"), QString::fromLatin1(modeName(s.mode)));
    o.insert(QStringLiteral("vibrancy"), s.vibrancy);
    QJsonObject ro;   // the resolved roles, for reference (import regenerates them from the seed)
    const QMap<QString, QColor> r = generate(s);
    for (auto it = r.constBegin(); it != r.constEnd(); ++it) ro.insert(it.key(), it.value().name());
    o.insert(QStringLiteral("roles"), ro);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

bool fromJson(const QByteArray &json, Spec *out, QString *error) {
    auto fail = [&](const QString &e) { if (error) *error = e; return false; };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull() || !doc.isObject()) return fail(QStringLiteral("Not a JSON theme file (%1)").arg(pe.errorString()));
    const QJsonObject o = doc.object();
    if (o.contains(QStringLiteral("format")) && o.value(QStringLiteral("format")).toString() != QLatin1String("libreworks-theme"))
        return fail(QStringLiteral("Not a %1 theme file").arg(brand::name()));
    if (o.value(QStringLiteral("version")).toInt(1) > 1) return fail(QStringLiteral("Theme file version is newer than this %1").arg(brand::name()));
    Spec s;
    QString seed = o.value(QStringLiteral("seed")).toString();
    if (seed.isEmpty()) seed = o.value(QStringLiteral("roles")).toObject().value(QStringLiteral("accent")).toString();
    const QColor sc = QColor::fromString(seed);
    if (!sc.isValid()) return fail(QStringLiteral("The theme file has no valid seed colour"));
    s.seed = sc.toRgb();
    s.seed.setAlpha(255);
    const QJsonValue mv = o.value(QStringLiteral("mode"));
    if (mv.isDouble()) s.mode = std::clamp(mv.toInt(), 0, 2);
    else if (mv.isString() && !parseMode(mv.toString(), &s.mode)) return fail(QStringLiteral("Unknown mode '%1'").arg(mv.toString()));
    if (o.contains(QStringLiteral("vibrancy"))) s.vibrancy = std::clamp(o.value(QStringLiteral("vibrancy")).toInt(), 0, 100);
    s.name = o.value(QStringLiteral("name")).toString().trimmed();
    if (s.name.isEmpty()) s.name = QStringLiteral("Imported");
    if (out) *out = s;
    return true;
}

double contrast(const QColor &a, const QColor &b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

QMap<QString, QColor> roles(const QString &id, const QColor &accentOverride) {
    const Source src = source(id);
    QMap<QString, QColor> r = src.r;
    QColor accent = accentOverride.isValid() ? accentOverride.toRgb() : src.accent;
    accent.setAlpha(255);
    if (accentOverride.isValid()) { r[QStringLiteral("tabAccent")] = accent; r[QStringLiteral("currentRow")] = mix(r[QStringLiteral("input")], accent, src.dark ? 0.3 : 0.15); }
    QColor at;
    accentPair(accent, at);
    r[QStringLiteral("accent")] = accent;
    r[QStringLiteral("accentText")] = at;
    r[QStringLiteral("disabled")] = disabledText(r[QStringLiteral("text")], r[QStringLiteral("window")]);
    r[QStringLiteral("pasteboard")] = src.pasteboard;
    r[QStringLiteral("phBot")] = r[QStringLiteral("pressed")];
    return r;
}

QColor pasteboard(const QString &id) { return source(id).pasteboard; }

static int g_schemeOverride = -1;
void setSystemSchemeOverride(int scheme) { g_schemeOverride = scheme; }
int systemScheme() {
    if (g_schemeOverride >= 0) return g_schemeOverride;
    if (auto *h = QGuiApplication::styleHints()) return int(h->colorScheme());
    return 0;
}

QString resolve(const QString &id, bool followSystem, int scheme) {
    const QString sel = exists(id) ? id : defaultId();
    if (!followSystem) return sel;
    if (scheme < 0) scheme = systemScheme();
    if (isGenerated(sel)) {   // a generated theme regenerates itself in the desktop's mode
        QString base = sel;
        if (base.endsWith(QLatin1String("@light"))) base.chop(6);
        else if (base.endsWith(QLatin1String("@dark"))) base.chop(5);
        if (scheme == int(Qt::ColorScheme::Dark)) return base + QStringLiteral("@dark");
        if (scheme == int(Qt::ColorScheme::Light)) return base + QStringLiteral("@light");
        return sel;
    }
    const bool selDark = findBase(sel)->dark;
    if (sel == QLatin1String("pink") && scheme == int(Qt::ColorScheme::Dark)) return QStringLiteral("pink-night");
    if (sel == QLatin1String("pink-night") && scheme == int(Qt::ColorScheme::Light)) return QStringLiteral("pink");
    if (scheme == int(Qt::ColorScheme::Dark) && !selDark) return sel == QLatin1String("solarized-light") ? QStringLiteral("solarized-dark") : QStringLiteral("dark");
    if (scheme == int(Qt::ColorScheme::Light) && selDark)
        return sel == QLatin1String("solarized-dark") ? QStringLiteral("solarized-light") : sel == QLatin1String("high-contrast") ? sel : defaultId();
    return sel;
}

// The stylesheet replaces the native combo / spin box arrows, so it needs arrow images. They are
// drawn here (no SVG plugin needed) at 1x and 2x, per arrow colour, into a per-process temp dir.
static QString arrowDir(const QColor &ink, const QColor &dis) {
    static QTemporaryDir root;   // removed at exit
    if (!root.isValid()) return root.path();
    // 0.3.15: always the same directory so the stylesheet URL stays constant across themes;
    // PNGs are overwritten when the arrow colours change (palette path avoids setStyleSheet).
    const QString dir = root.filePath(QStringLiteral("arrows"));
    static QString lastKey;
    const QString key = ink.name().mid(1) + QLatin1Char('-') + dis.name().mid(1);
    if (QDir(dir).exists() && lastKey == key) return dir;
    lastKey = key;
    QDir().mkpath(dir);
    struct Variant { const char *name; QColor color; bool up; };
    const QColor hover = mix(ink, ink.lightness() > 128 ? Qt::white : Qt::black, 0.5);
    const Variant vs[] = {
        {"down", ink, false}, {"down-hover", hover, false}, {"down-open", ink, false}, {"down-disabled", dis, false},
        {"up", ink, true}, {"up-disabled", dis, true},
    };
    for (const Variant &v : vs)
        for (int scale : {1, 2}) {
            QImage img(7 * scale, 4 * scale, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            p.scale(scale, scale);
            p.setPen(Qt::NoPen);
            p.setBrush(v.color);
            QPainterPath tri;
            if (v.up) { tri.moveTo(0, 4); tri.lineTo(7, 4); tri.lineTo(3.5, 0); }
            else { tri.moveTo(0, 0); tri.lineTo(7, 0); tri.lineTo(3.5, 4); }
            tri.closeSubpath();
            p.drawPath(tri);
            p.end();
            img.save(QDir(dir).filePath(QString::fromLatin1(v.name) + (scale == 2 ? QStringLiteral("@2x.png") : QStringLiteral(".png"))));
        }
    return dir;
}

// v0.2.7: no dotted focus rectangle around the selected tab's label (every QTabBar / QTabWidget, every
// theme). Keyboard focus still works: the arrow keys / Ctrl+Tab move the tab, the selected tab shows it.
static QString g_current;
static QString g_appliedKey;
static QString g_appliedCss;
static QHash<QString, QString> g_cssCache; // cacheKey -> fully resolved stylesheet
static QMap<QString, QColor> g_roles;

// App palette: Fusion chrome (Light/Midlight/Button = light/body/button) plus
// general roles. Header / panel / tool tokens live on per-widget palettes so
// the one palette() stylesheet can stay theme-independent.
static QPalette g_appPalette;
static QHash<QString, QPalette> g_paletteCache;
static QSet<QWidget *> *g_justStyled = nullptr;

static QColor roleColor(const char *key) {
    return g_roles.value(QString::fromLatin1(key));
}

static QPalette withRoles(const QPalette &base, std::initializer_list<std::pair<QPalette::ColorRole, const char *>> xs)
{
    QPalette p = base;
    for (const auto &x : xs)
        p.setColor(x.first, roleColor(x.second));
    return p;
}

static bool isDockTitleButton(const QWidget *w)
{
    if (!w || !qobject_cast<const QAbstractButton *>(w)) return false;
    if (w->width() > 40 || w->height() > 40) return false;
    for (const QWidget *p = w->parentWidget(); p; p = p->parentWidget()) {
        if (auto *d = qobject_cast<const QDockWidget *>(p))
            return d->titleBarWidget() == nullptr;
    }
    return false;
}

// Per-widget token sheets. QStyleSheetStyle resolves palette() against the
// application palette only, so these are the 0.3.14 rules with baked @token@
// colours. setStyleSheet runs only when the resolved text changes (a theme
// switch), and only on this widget — never on QMainWindow / the app.
static QString resolveTokens(QString s)
{
    for (auto it = g_roles.constBegin(); it != g_roles.constEnd(); ++it)
        s.replace(QLatin1Char('@') + it.key() + QLatin1Char('@'), it.value().name());
    return s;
}
static void setSheetIfChanged(QWidget *w, const char *tmpl)
{
    const QString css = resolveTokens(QString::fromLatin1(tmpl));
    if (w->styleSheet() != css) {
        w->setStyleSheet(css);
        if (g_justStyled) g_justStyled->insert(w);
    }
    w->setAttribute(Qt::WA_StyledBackground, true);
}

static const char *kSheetDockTitleW = R"(
#DockTitle {
  font-weight: bold; border: 1px solid @edge@;
  background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 @hdrTop@, stop:1 @hdrBot@);
}
)";
static const char *kSheetToolsW = R"(
#ToolsPanel { background: @tools@; }
#ToolsPanel QToolButton { border: 1px solid transparent; border-radius: 0px; padding: 1px; background: transparent; }
#ToolsPanel QToolButton:hover { border: 1px solid @hovEdge@; background: @hovBg@; }
#ToolsPanel QToolButton:checked { border: 1px solid @chkEdge@; background: @chkBg@; }
)";
static const char *kSheetToolSectionW = R"(
#ToolSection {
  font-size: 8pt; color: @secText@; padding: 1px 0px; qproperty-alignment: AlignCenter;
  background: @secBg@; border-top: 1px solid @secHi@; border-bottom: 1px solid @secLo@;
}
)";
static const char *kSheetPanelHeaderW = R"(
#PanelHeader {
  text-align: left; font-weight: bold; padding: 3px 4px; border: 1px solid @edge@;
  background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 @phTop@, stop:1 @pressed@);
}
#PanelHeader:hover { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 @phHovTop@, stop:1 @phHovBot@);
}
)";
static const char *kSheetListW = R"(
QListWidget, QTreeWidget { background: @input@; border: 1px solid @inputEdge@; alternate-background-color: @altBase@; color: @text@; }
QListWidget::item, QTreeWidget::item { color: @text@; }
QListWidget::item:selected, QTreeWidget::item:selected { background: @accent@; color: @accentText@; }
)";
static const char *kSheetTipW = R"(
QTipLabel, QToolTip { background: @tipBg@; color: @tipText@; border: 1px solid @frame@; }
)";

static void applyWidgetPalette(QWidget *w)
{
    if (!w || g_roles.isEmpty() || g_appPalette.resolveMask() == 0) return;
    QPalette p = g_appPalette;
    const QString name = w->objectName();
    const char *sheet = nullptr;
    if (w->inherits("QTipLabel")) {
        p = withRoles(g_appPalette, {
            {QPalette::Base, "tipBg"},
            {QPalette::Text, "tipText"},
            {QPalette::WindowText, "tipText"},
            {QPalette::Window, "tipBg"},
            {QPalette::ToolTipBase, "tipBg"},
            {QPalette::ToolTipText, "tipText"},
            {QPalette::Dark, "frame"}
        });
        sheet = kSheetTipW;
    } else if (isDockTitleButton(w)) {
        // Keep the 0.3.14 Fusion mapping; do not inherit the dock's header-token palette.
        p = g_appPalette;
    } else if (auto *d = qobject_cast<QDockWidget *>(w)) {
        if (!d->titleBarWidget()) {
            p = withRoles(g_appPalette, {
                {QPalette::Light, "hdrTop"}, {QPalette::Midlight, "hdrBot"}, {QPalette::Dark, "edge"}
            });
            // Title colours come from the once-set [lwTheme] app rules; only the
            // property changes on a switch (no setStyleSheet on the dock).
            if (w->property("lwTheme").toString() != g_current)
                w->setProperty("lwTheme", g_current);
        }
    } else if (name.startsWith(QLatin1String("panel_"))) {
        if (w->property("lwTheme").toString() != g_current)
            w->setProperty("lwTheme", g_current);
        p = withRoles(g_appPalette, {{QPalette::Window, "window"}});
        w->setAutoFillBackground(true);
    } else if (name == QLatin1String("DockTitle")) {
        p = withRoles(g_appPalette, {
            {QPalette::Light, "hdrTop"}, {QPalette::Midlight, "hdrBot"}, {QPalette::Dark, "edge"}
        });
        sheet = kSheetDockTitleW;
    } else if (name == QLatin1String("ToolsPanel")) {
        p = withRoles(g_appPalette, {
            {QPalette::Window, "tools"},
            {QPalette::Dark, "hovEdge"},
            {QPalette::AlternateBase, "hovBg"},
            {QPalette::Shadow, "chkEdge"},
            {QPalette::Midlight, "chkBg"}
        });
        sheet = kSheetToolsW;
    } else if (name == QLatin1String("ToolSection")) {
        p = withRoles(g_appPalette, {
            {QPalette::WindowText, "secText"},
            {QPalette::Window, "secBg"},
            {QPalette::Light, "secHi"},
            {QPalette::Mid, "secLo"}
        });
        sheet = kSheetToolSectionW;
    } else if (name == QLatin1String("PanelHeader")) {
        p = withRoles(g_appPalette, {
            {QPalette::Light, "phTop"},
            {QPalette::Mid, "pressed"},
            {QPalette::Dark, "edge"},
            {QPalette::AlternateBase, "phHovTop"},
            {QPalette::Midlight, "phHovBot"}
        });
        sheet = kSheetPanelHeaderW;
    } else if (name == QLatin1String("PanelBody")) {
        p = withRoles(g_appPalette, {
            {QPalette::Midlight, "body"},
            {QPalette::Window, "body"},
            {QPalette::Dark, "bodyEdge"}
        });
        w->setAutoFillBackground(true);
        w->setAttribute(Qt::WA_StyledBackground, true);
    } else if (name == QLatin1String("PropertyInspector")) {
        p = withRoles(g_appPalette, {{QPalette::Window, "pi"}});
        w->setAutoFillBackground(true);
        if (w->property("lwTheme").toString() != g_current)
            w->setProperty("lwTheme", g_current);
    } else if (name == QLatin1String("ViewTabRow")) {
        p = withRoles(g_appPalette, {{QPalette::Window, "hdrBot"}});
        w->setAutoFillBackground(true);
    } else if (qobject_cast<QListWidget *>(w) || qobject_cast<QTreeWidget *>(w)) {
        // Border is @inputEdge@; only this view is restyled, not a parent dock.
        sheet = kSheetListW;
    } else if (qobject_cast<QScrollBar *>(w)) {
        return;
    } else if (w->testAttribute(Qt::WA_SetPalette)) {
        // Internal dock layout wrappers can keep a copied palette from the
        // first apply; send them back to the current app palette.
        p = g_appPalette;
    } else {
        return; // inherit the app / parent palette
    }
    if (sheet)
        setSheetIfChanged(w, sheet);
    w->setPalette(p);
    w->update();
}

static void applyWidgetPalettesRecursive(QWidget *w)
{
    if (!w) return;
    applyWidgetPalette(w);
    const auto kids = w->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
    for (QWidget *c : kids)
        applyWidgetPalettesRecursive(c);
}


// Wrap long tooltips at ~400 px; leave short single-line tips unchanged.
static void wrapTipLabel(QLabel *lab)
{
    if (!lab) return;
    const int maxW = 400;
    const QString text = lab->text();
    if (text.isEmpty()) return;
    const QFontMetrics fm(lab->font());
    int contentW = 0;
    if (Qt::mightBeRichText(text)) {
        QTextDocument doc;
        doc.setHtml(text);
        doc.setDefaultFont(lab->font());
        contentW = int(std::ceil(doc.idealWidth()));
    } else {
        const auto lines = text.split(QLatin1Char('\n'));
        for (const QString &line : lines)
            contentW = qMax(contentW, fm.horizontalAdvance(line));
    }
    const QMargins mg = lab->contentsMargins();
    const int frame = mg.left() + mg.right() + 2 * lab->frameWidth() + 16;
    if (contentW + frame <= maxW) {
        if (lab->wordWrap() || lab->maximumWidth() == maxW) {
            lab->setWordWrap(false);
            lab->setMaximumWidth(QWIDGETSIZE_MAX);
            lab->setMinimumWidth(0);
            lab->adjustSize();
        }
        return;
    }
    if (lab->wordWrap() && lab->maximumWidth() == maxW && lab->width() == maxW)
        return;
    lab->setWordWrap(true);
    lab->setFixedWidth(maxW);
    lab->adjustSize();
}

class ThemeEventFilter final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override {
        if (!o->inherits("QTipLabel")) return false;
        const auto t = e->type();
        if (t != QEvent::Show && t != QEvent::PolishRequest && t != QEvent::Polish
            && t != QEvent::StyleChange)
            return false;
        if (auto *w = qobject_cast<QWidget *>(o)) {
            applyWidgetPalette(w);
            if (auto *lab = qobject_cast<QLabel *>(w))
                wrapTipLabel(lab);
            // QToolTip may assign its own sheet/palette after Polish/Show;
            // re-apply once the label is on screen so a reused tip cannot
            // keep a previous theme's text colour.
            if (t == QEvent::Show) {
                QPointer<QWidget> pw(w);
                QTimer::singleShot(0, w, [pw] {
                    if (!pw) return;
                    applyWidgetPalette(pw);
                    if (auto *lab = qobject_cast<QLabel *>(pw.data()))
                        wrapTipLabel(lab);
                });
            }
        }
        return false;
    }
};

static void ensureThemeFilter(QApplication &app) {
    static QPointer<ThemeEventFilter> f;
    if (!f) {
        f = new ThemeEventFilter(&app);
        app.installEventFilter(f);
    }
    uiutil::installEnableLabelFilter(&app);
    combofit::install(&app);
}

class NoTabFocusStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    void polish(QWidget *w) override {
        QProxyStyle::polish(w);
        applyWidgetPalette(w);
    }
    template<typename Fn>
    static void withFusionPalette(const QStyleOption *opt, const QWidget *w, Fn fn) {
        if (!opt || !isDockTitleButton(w)) { fn(opt); return; }
        // Patch Light/Midlight/Button back to the 0.3.14 Fusion chrome colours.
        if (auto *tb = qstyleoption_cast<const QStyleOptionToolButton *>(opt)) {
            QStyleOptionToolButton o(*tb); o.palette = g_appPalette; fn(&o); return;
        }
        if (auto *tb = qstyleoption_cast<const QStyleOptionTitleBar *>(opt)) {
            QStyleOptionTitleBar o(*tb); o.palette = g_appPalette; fn(&o); return;
        }
        if (auto *b = qstyleoption_cast<const QStyleOptionButton *>(opt)) {
            QStyleOptionButton o(*b); o.palette = g_appPalette; fn(&o); return;
        }
        if (auto *cx = qstyleoption_cast<const QStyleOptionComplex *>(opt)) {
            QStyleOptionComplex o(*cx); o.palette = g_appPalette; fn(&o); return;
        }
        QStyleOption o(*opt); o.palette = g_appPalette; fn(&o);
    }
    static bool isTab(const QWidget *w) { return w && (qobject_cast<const QTabBar *>(w) || w->inherits("QTabWidget")); }
    // v0.3.1: item views (Styles, Swatches, Auto Shapes, Library, Layers, Frames, History, every list /
    // tree / table) show only the selection highlight: no dotted focus rect and no dashed outline round
    // the current item (in icon mode that was a small dashed box under the selected style). The current
    // index still exists, so the arrow keys, Home / End and type-ahead keep working.
    static bool isItemView(const QWidget *w) {
        for (int i = 0; w && i < 2; ++i, w = w->parentWidget())   // the view or its viewport
            if (qobject_cast<const QAbstractItemView *>(w)) return true;
        return false;
    }
    static bool noFocus(const QWidget *w) { return isTab(w) || isItemView(w); }
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override {
        if (pe == PE_Widget && w && w->objectName() == QLatin1String("PanelBody") && opt) {
            const QRect r = opt->rect;
            p->fillRect(r, w->palette().color(QPalette::Window));
            p->setPen(w->palette().color(QPalette::Dark));
            p->drawLine(r.topLeft(), r.bottomLeft());
            p->drawLine(r.topRight(), r.bottomRight());
            p->drawLine(r.bottomLeft(), r.bottomRight());
            return;
        }
        if (pe == PE_FrameFocusRect && noFocus(w)) return;
        if ((pe == PE_PanelItemViewItem || pe == PE_PanelItemViewRow) && opt && (opt->state & State_HasFocus)) {
            if (auto *vi = qstyleoption_cast<const QStyleOptionViewItem *>(opt)) {
                QStyleOptionViewItem o(*vi);
                o.state &= ~State_HasFocus;
                QProxyStyle::drawPrimitive(pe, &o, p, w);
                return;
            }
        }
        withFusionPalette(opt, w, [&](const QStyleOption *o) { QProxyStyle::drawPrimitive(pe, o, p, w); });
    }
    void drawDockTitle(const QStyleOption *opt, QPainter *p, const QWidget *w) const {
        const QRect r = opt->rect;
        const QPalette pal = w ? w->palette() : (opt ? opt->palette : g_appPalette);
        QLinearGradient g(r.topLeft(), QPointF(r.left(), r.bottom() + 1));
        g.setColorAt(0, pal.color(QPalette::Light));
        g.setColorAt(1, pal.color(QPalette::Midlight));
        p->fillRect(r, g);
        p->setPen(pal.color(QPalette::Dark));
        p->drawRect(r.adjusted(0, 0, -1, -1));
        QRect tr = r.adjusted(6, 3, -6, -3);
        if (const auto *d = qobject_cast<const QDockWidget *>(w)) {
            for (auto *b : d->findChildren<QAbstractButton *>()) {
                if (!b->isVisible() || b->width() > 40 || b->height() > 40) continue;
                const QRect br = b->geometry();
                if (br.intersects(r))
                    tr.setRight(qMin(tr.right(), br.left() - 4));
            }
        }
        QFont f = w ? w->font() : p->font();
        f.setBold(true);
        p->setFont(f);
        p->setPen(pal.color(QPalette::WindowText));
        QString title;
        if (auto *dw = qstyleoption_cast<const QStyleOptionDockWidget *>(opt))
            title = dw->title;
        else if (const auto *d = qobject_cast<const QDockWidget *>(w))
            title = d->windowTitle();
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, title);
    }
    void drawControl(ControlElement ce, const QStyleOption *opt, QPainter *p, const QWidget *w) const override {
        if (ce == CE_DockWidgetTitle && opt) {
            drawDockTitle(opt, p, w);
            return;
        }
        if (ce == CE_ItemViewItem && opt && (opt->state & State_HasFocus)) {
            if (auto *vi = qstyleoption_cast<const QStyleOptionViewItem *>(opt)) {
                QStyleOptionViewItem o(*vi);
                o.state &= ~State_HasFocus;
                QProxyStyle::drawControl(ce, &o, p, w);
                return;
            }
        }
        if ((ce == CE_TabBarTab || ce == CE_TabBarTabLabel || ce == CE_TabBarTabShape) && opt && (opt->state & State_HasFocus)) {
            if (auto *tab = qstyleoption_cast<const QStyleOptionTab *>(opt)) {
                QStyleOptionTab o(*tab);
                o.state &= ~State_HasFocus;
                QProxyStyle::drawControl(ce, &o, p, w);
                return;
            }
        }
        if (ce == CE_ComboBoxLabel && opt) {
            if (auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(opt)) {
                QStyleOptionComboBox o(*cb);
                const QRect tr = subControlRect(CC_ComboBox, &o, SC_ComboBoxEditField, w);
                if (tr.width() > 0 && !o.currentText.isEmpty()) {
                    const QString elided = QFontMetrics(w ? w->font() : p->font())
                                               .elidedText(o.currentText, Qt::ElideMiddle, tr.width());
                    o.currentText = elided;
                }
                withFusionPalette(&o, w, [&](const QStyleOption *oo) { QProxyStyle::drawControl(ce, oo, p, w); });
                return;
            }
        }
        withFusionPalette(opt, w, [&](const QStyleOption *o) { QProxyStyle::drawControl(ce, o, p, w); });
    }
    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *opt, QPainter *p, const QWidget *w) const override {
        if (isDockTitleButton(w)) {
            withFusionPalette(opt, w, [&](const QStyleOption *o) {
                if (auto *cx = qstyleoption_cast<const QStyleOptionComplex *>(o))
                    QProxyStyle::drawComplexControl(cc, cx, p, w);
                else
                    QProxyStyle::drawComplexControl(cc, opt, p, w);
            });
            return;
        }
        QProxyStyle::drawComplexControl(cc, opt, p, w);
    }
    // 0.3.14 used `QDockWidget { titlebar-close-icon: none; }`. The buttons are
    // still real widgets; we just don't paint a close pixmap via the style.
    QIcon standardIcon(StandardPixmap sp, const QStyleOption *opt, const QWidget *w) const override {
        if (sp == SP_TitleBarCloseButton && w && qobject_cast<const QDockWidget *>(w->parentWidget()))
            return QIcon();
        if (sp == SP_DockWidgetCloseButton)
            return QIcon();
        return QProxyStyle::standardIcon(sp, opt, w);
    }
};

static QPointer<QStyle> &installedStyle() { static QPointer<QStyle> p; return p; }
int proxyStyleCount() {
    int n = 0;
    for (QStyle *s : qApp->findChildren<QStyle *>()) if (dynamic_cast<NoTabFocusStyle *>(s)) ++n;
    return n;
}

static QString cacheKey(const QString &id, const QColor &accent) {
    QString k = id + QLatin1Char('|') + (accent.isValid() ? accent.name() : QString());
    Spec sp;
    if (specFor(id, &sp)) k += QStringLiteral("|%1|%2|%3").arg(sp.seed.name()).arg(sp.mode).arg(sp.vibrancy);
    return k;
}

static QString dockTitleRules()
{
    QString s;
    s += QStringLiteral("\nQDockWidget { titlebar-close-icon: none; }\n");
    const QString tmpl = QStringLiteral(
        "QWidget[lwTheme=\"%1\"] { background: %5; color: %6; }\n""#PropertyInspector[lwTheme=\"%1\"] { background: %5; }\n"
        "QDockWidget[lwTheme=\"%1\"]::title {\n"
        "  text-align: left; padding: 3px 6px; font-weight: bold;\n"
        "  background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 %2, stop:1 %3);\n"
        "  border: 1px solid %4;\n"
        "}\n");
    for (const Info &info : list()) {
        const QMap<QString, QColor> r = roles(info.id);
        s += tmpl.arg(info.id,
                      r.value(QStringLiteral("hdrTop")).name(),
                      r.value(QStringLiteral("hdrBot")).name(),
                      r.value(QStringLiteral("edge")).name(),
                      r.value(QStringLiteral("window")).name(),
                      r.value(QStringLiteral("text")).name());
    }
    return s;
}

void apply(QApplication &app) { apply(app, defaultId()); }

void apply(QApplication &app, const QString &idIn, const QColor &accent) {
    const QString id = exists(idIn) ? idIn : defaultId();
    const QMap<QString, QColor> r = roles(id, accent);
    // v0.3.2: install the proxy style once. Once a stylesheet is set, app.style() is Qt's QStyleSheetStyle
    // wrapping our proxy, so the old dynamic_cast check failed on every switch and setStyle() re-created
    // the style and re-polished every widget on top of the stylesheet re-polish.
    if (!installedStyle()) {   // QApplication deletes a replaced style, which clears the QPointer
        if (auto *s = QStyleFactory::create(QStringLiteral("Windows"))) {
            auto *proxy = new NoTabFocusStyle(s);
            app.setStyle(proxy);
            installedStyle() = proxy;
        }
    }
    const QString palKey = cacheKey(id, accent);
    auto c = [&](const char *k) { return r.value(QString::fromLatin1(k)); };
    QPalette p;
    if (g_paletteCache.contains(palKey)) {
        p = g_paletteCache.value(palKey);
    } else {
    // Palette is still filled so unstyled / Fusion chrome (dock title buttons, native fallbacks)
    // matches 0.3.14. The stylesheet uses @token@ colours again so every theme is pixel-exact;
    // resolved CSS is cached and setStyleSheet is skipped when it is unchanged (same theme).
    // The 0.3.15 complete QScrollBar rules stay, coloured with the 0.3.14 tokens.
    p.setColor(QPalette::Window, c("window"));
    p.setColor(QPalette::WindowText, c("text"));
    p.setColor(QPalette::Text, c("text"));
    p.setColor(QPalette::Base, c("input"));
    p.setColor(QPalette::AlternateBase, c("altBase"));
    p.setColor(QPalette::Button, c("button"));
    p.setColor(QPalette::ButtonText, c("text"));
    p.setColor(QPalette::BrightText, c("accentText"));
    p.setColor(QPalette::Highlight, c("accent"));
    p.setColor(QPalette::HighlightedText, c("accentText"));
    // 0.3.14 mapping (restored): Fusion chrome (dock float/close, etc.) reads Light/Midlight/Button.
    // Header gradients stay on @hdrTop@/@hdrBot@ in the stylesheet, not these roles.
    p.setColor(QPalette::Light, c("light"));
    p.setColor(QPalette::Midlight, c("body"));
    p.setColor(QPalette::Mid, c("line"));
    p.setColor(QPalette::Dark, c("edge"));
    p.setColor(QPalette::Shadow, c("chkEdge"));
    p.setColor(QPalette::ToolTipBase, c("tipBg"));
    p.setColor(QPalette::ToolTipText, c("tipText"));
    p.setColor(QPalette::PlaceholderText, c("disabled"));
    p.setColor(QPalette::Link, c("tabAccent"));
    p.setColor(QPalette::LinkVisited, c("frame"));
    for (QPalette::ColorRole role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText, QPalette::PlaceholderText})
        p.setColor(QPalette::Disabled, role, c("disabled"));
    p.setColor(QPalette::Disabled, QPalette::Base, c("dropDis"));
    p.setColor(QPalette::Disabled, QPalette::Highlight, c("line"));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, c("text"));
        g_paletteCache.insert(palKey, p);
    }
    g_roles = r;
    g_current = id;
    g_appPalette = p;
    app.setPalette(p);
    const QString key = cacheKey(id, accent);
    // Always rewrite arrow PNGs for this theme's ink (stable directory; CSS URL unchanged).
    const QString arrows = QDir::fromNativeSeparators(arrowDir(c("arrow"), c("disabled")));
    // The palette() stylesheet (including the 0.3.14 chrome rules) is identical
    // across themes, so setStyleSheet runs once. Later switches set the palette
    // and per-widget token palettes, then re-polish so QStyleSheetStyle
    // re-resolves palette() and [lwTheme] against the new colours.
    QString css = g_cssCache.value(QStringLiteral("palette"));
    if (css.isEmpty()) {
        css = QString::fromLatin1(kStyle);
        css.replace(QStringLiteral("@ARROWS@"), arrows);
        css += dockTitleRules();
        if (css.contains(QLatin1Char('@'))) qWarning("theme %s: unresolved stylesheet token", qPrintable(id));
        g_cssCache.insert(QStringLiteral("palette"), css);
    }
    g_appliedKey = key;
    if (css != g_appliedCss) {
        app.setStyleSheet(css);
        g_appliedCss = css;
    }
    ensureThemeFilter(app);
    // Palettes and [lwTheme] properties first, then unpolish every widget.
    // QStyleSheetStyle caches palette() at polish time and does not refresh
    // on setPalette alone, so a dock-only restyle left the menubar, menus,
    // toolbar, status bar, tabs, buttons and other app-stylesheet chrome
    // stuck on the theme they were first polished with.
    const QWidgetList tops = app.topLevelWidgets();
    for (QWidget *w : tops)
        w->setUpdatesEnabled(false);
    QSet<QWidget *> justStyled;
    g_justStyled = &justStyled;
    for (QWidget *w : tops)
        applyWidgetPalettesRecursive(w);
    g_justStyled = nullptr;
    const QWidgetList widgets = app.allWidgets();
    for (QWidget *w : widgets) {
        if (!w) continue;
        // Custom-painted canvas reads theme::color() in paintEvent; skip unpolish.
        if (w->inherits("Canvas")) continue;
        // setStyleSheet already unpolished / re-polished these token widgets.
        if (justStyled.contains(w)) continue;
        if (QStyle *st = w->style()) { st->unpolish(w); st->polish(w); }
    }
    for (QWidget *w : widgets) {
        if (w && w->inherits("QTipLabel"))
            applyWidgetPalette(w);
    }
    for (QWidget *w : tops) {
        w->setUpdatesEnabled(true);
        w->update();
    }
}

void applyPrefs(const Prefs &pr) {
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app) return;
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        if (auto *h = QGuiApplication::styleHints())
            QObject::connect(h, &QStyleHints::colorSchemeChanged, app, [] {
                if (appState().prefs.themeFollowSystem) appState().setPrefs(appState().prefs);
            });
    }
    syncGenerated(pr);
    const QString id = resolve(pr.theme, pr.themeFollowSystem);
    const QColor accent = pr.accentOverride ? pr.accentColor : QColor();
    if (cacheKey(id, accent) == g_appliedKey) return;
    apply(*app, id, accent);
}

QString current() { return g_current.isEmpty() ? defaultId() : g_current; }

QColor color(const char *role) {
    if (g_roles.isEmpty()) g_roles = roles(defaultId());
    return g_roles.value(QString::fromLatin1(role), QColor(Qt::magenta));
}

}
