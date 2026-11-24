#include "ColorPicker.h"
#include "PopupSlider.h"
#include "Icons.h"
#include <QApplication>
#include <cmath>
#include <QColorDialog>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QSlider>
#include <QHideEvent>
#include <QRegularExpressionValidator>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#ifdef LW_HAVE_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QRandomGenerator>
#endif

namespace {
QPointer<ScreenColorPicker> g_active;
constexpr int kZoom = 7, kCells = 11;   // the magnified view: 11 x 11 pixels, 7x
}

ScreenColorPicker::Mode ScreenColorPicker::autoMode() {
    const QByteArray forced = qgetenv("LW_COLOR_PICKER").toLower();
    if (forced == "screen") return Screen;
    if (forced == "portal") return Portal;
    if (forced == "app") return AppWindows;
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
#ifdef LW_HAVE_DBUS
        QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
        if (bus && bus->isServiceRegistered(QStringLiteral("org.freedesktop.portal.Desktop"))) return Portal;
#endif
        return AppWindows;
    }
    // headless platforms have no screen to grab (offscreen returns a window's backing store)
    const QString plat = QGuiApplication::platformName();
    if (plat == QLatin1String("offscreen") || plat == QLatin1String("minimal")) return AppWindows;
    return Screen;
}

ScreenColorPicker *ScreenColorPicker::active() { return g_active; }

ScreenColorPicker *ScreenColorPicker::start(QWidget *owner, Mode mode) {
    if (g_active) g_active->cancel();
    if (mode == Auto) mode = autoMode();
    auto *p = new ScreenColorPicker(owner, mode);
    g_active = p;
    if (mode == Portal) {
        if (!p->startPortal()) { p->m_mode = AppWindows; mode = AppWindows; }
        else return p;
    }
    if (mode == Screen && !p->captureScreens()) p->m_mode = AppWindows;
    if (p->m_mode == AppWindows) p->captureAppWindows();
    // the preview is a pop-up: it gets every move, click and key (also over windows a modal
    // dialog blocks, and with a pointer grab on X11 anywhere on the screen)
    p->setCursor(Qt::CrossCursor);
    p->setMouseTracking(true);
    p->m_pos = QCursor::pos();
    p->hoverAt(p->m_pos);
    // report the colour under the pointer once the caller has connected (the start-up
    // sample above is emitted before anyone can listen)
    p->m_cur = QColor();
    QMetaObject::invokeMethod(p, [p] { if (!p->m_cur.isValid()) p->hoverAt(p->m_pos); }, Qt::QueuedConnection);
    p->show();
    p->raise();
    p->setFocus(Qt::PopupFocusReason);
    p->grabMouse(Qt::CrossCursor);
    p->grabKeyboard();
    return p;
}

ScreenColorPicker::ScreenColorPicker(QWidget *owner, Mode mode)
    : QWidget(owner ? owner->window() : nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
      m_mode(mode), m_owner(owner) {
    setObjectName(QStringLiteral("colorPickerPreview"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_NoMouseReplay);
    setFixedSize(kCells * kZoom + 2 + 8, kCells * kZoom + 2 + 8 + 22);
    if (owner) connect(owner, &QObject::destroyed, this, [this] { cancel(); });
}

bool ScreenColorPicker::captureScreens() {
    m_screens.clear();
    for (QScreen *s : QGuiApplication::screens()) {
        const QPixmap px = s->grabWindow(0);
        if (px.isNull()) return false;
        Shot sh;
        sh.geom = s->geometry();
        sh.img = px.toImage().convertToFormat(QImage::Format_ARGB32);
        // device pixels per logical pixel, from the grab itself (exact on fractional scales)
        sh.dpr = sh.geom.width() > 0 ? qreal(sh.img.width()) / sh.geom.width() : s->devicePixelRatio();
        // a grab that is not the whole screen (some platforms return a window instead) would
        // put samples in the wrong place: let the caller fall back to the app's windows
        if (sh.dpr <= 0 || std::abs(sh.img.height() - sh.geom.height() * sh.dpr) > 1.0) return false;
        m_screens << sh;
    }
    return !m_screens.isEmpty();
}

void ScreenColorPicker::captureAppWindows() {
    m_windows.clear();
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w == this || !w->isVisible() || w->windowType() == Qt::ToolTip) continue;
        Shot sh;
        const QPixmap px = w->grab();
        sh.img = px.toImage().convertToFormat(QImage::Format_ARGB32);
        sh.dpr = px.devicePixelRatio();
        sh.geom = QRect(w->mapToGlobal(QPoint(0, 0)), w->size());
        m_windows.insert(w, sh);
    }
}

QColor ScreenColorPicker::sampleImage(const QImage &img, qreal dpr, const QPoint &local) {
    if (img.isNull()) return QColor();
    const QPoint d(qBound(0, int(std::floor((local.x() + 0.5) * dpr)), img.width() - 1),
                   qBound(0, int(std::floor((local.y() + 0.5) * dpr)), img.height() - 1));
    return QColor::fromRgb(img.pixel(d));
}

QColor ScreenColorPicker::sampleAt(const QPoint &g) const {
    if (m_mode == Screen) {
        for (const Shot &s : m_screens)
            if (s.geom.contains(g) && !s.img.isNull()) return sampleImage(s.img, s.dpr, g - s.geom.topLeft());
        return QColor();
    }
    // the app window under the pointer (the topmost one), else any window containing it
    QWidget *w = QApplication::widgetAt(g);
    if (w == this || (w && w->window() == this)) w = nullptr;
    if (w) {
        auto it = m_windows.constFind(w->window());
        if (it != m_windows.constEnd() && !it->img.isNull()) return sampleImage(it->img, it->dpr, w->window()->mapFromGlobal(g));
    }
    for (auto it = m_windows.cbegin(); it != m_windows.cend(); ++it)
        if (it->geom.contains(g) && !it->img.isNull()) return sampleImage(it->img, it->dpr, g - it->geom.topLeft());
    return QColor();
}

void ScreenColorPicker::hoverAt(const QPoint &g) {
    m_pos = g;
    const QColor c = sampleAt(g);
    if (c.isValid() && c != m_cur) { m_cur = c; emit hovered(c); }
    // keep the preview beside the pointer (never under it), on the pointer's screen
    QPoint at = g + QPoint(18, 18);
    if (QScreen *s = QGuiApplication::screenAt(g)) {
        const QRect sg = s->availableGeometry();
        if (at.x() + width() > sg.right()) at.setX(g.x() - 18 - width());
        if (at.y() + height() > sg.bottom()) at.setY(g.y() - 18 - height());
    }
    move(at);
    update();
}

void ScreenColorPicker::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x30, 0x30, 0x30));
    const QRect mag(4, 4, kCells * kZoom + 2, kCells * kZoom + 2);
    // magnified neighbourhood
    for (int y = 0; y < kCells; ++y)
        for (int x = 0; x < kCells; ++x) {
            const QColor c = sampleAt(m_pos + QPoint(x - kCells / 2, y - kCells / 2));
            p.fillRect(QRect(mag.left() + 1 + x * kZoom, mag.top() + 1 + y * kZoom, kZoom, kZoom), c.isValid() ? c : QColor(0x50, 0x50, 0x50));
        }
    p.setPen(Qt::white);
    p.drawRect(mag.adjusted(0, 0, -1, -1));
    const QRect centre(mag.left() + 1 + (kCells / 2) * kZoom - 1, mag.top() + 1 + (kCells / 2) * kZoom - 1, kZoom + 1, kZoom + 1);
    p.setPen(Qt::black); p.drawRect(centre);
    p.setPen(Qt::white); p.drawRect(centre.adjusted(-1, -1, 1, 1));
    // swatch + hex
    const QRect sw(4, mag.bottom() + 4, 18, 16);
    p.fillRect(sw, m_cur.isValid() ? m_cur : QColor(Qt::transparent));
    p.setPen(Qt::white);
    p.drawRect(sw.adjusted(0, 0, -1, -1));
    p.drawText(QRect(sw.right() + 6, sw.top(), width() - sw.right() - 10, sw.height()), Qt::AlignVCenter | Qt::AlignLeft,
               m_cur.isValid() ? m_cur.name(QColor::HexRgb).toUpper() : QStringLiteral("—"));
}

void ScreenColorPicker::mouseMoveEvent(QMouseEvent *e) { hoverAt(e->globalPosition().toPoint()); }

void ScreenColorPicker::mousePressEvent(QMouseEvent *e) {
    e->accept();
    if (e->button() == Qt::RightButton) { cancel(); return; }
    if (e->button() != Qt::LeftButton) return;
    const QColor c = sampleAt(e->globalPosition().toPoint());
    if (c.isValid()) pick(c); else cancel();
}
void ScreenColorPicker::mouseReleaseEvent(QMouseEvent *e) { e->accept(); }

void ScreenColorPicker::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) { cancel(); return; }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { if (m_cur.isValid()) pick(m_cur); return; }
    // arrow keys nudge the pointer one pixel for exact picks
    QPoint d;
    if (e->key() == Qt::Key_Left) d = {-1, 0};
    else if (e->key() == Qt::Key_Right) d = {1, 0};
    else if (e->key() == Qt::Key_Up) d = {0, -1};
    else if (e->key() == Qt::Key_Down) d = {0, 1};
    if (!d.isNull()) { QCursor::setPos(m_pos + d); hoverAt(m_pos + d); }
}

void ScreenColorPicker::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    // a pop-up closed by the system (focus loss, a click Qt routes elsewhere) cancels
    if (!m_done && m_mode != Portal) QTimer::singleShot(0, this, [this] { if (!m_done) cancel(); });
}

void ScreenColorPicker::finish() {
    m_done = true;
    if (g_active == this) g_active = nullptr;
    releaseMouse();
    releaseKeyboard();
    hide();
    deleteLater();
}

void ScreenColorPicker::cancel() {
    if (m_done) return;
    finish();
    emit cancelled();
}

void ScreenColorPicker::pick(const QColor &c) {
    if (m_done) return;
    m_cur = c;
    finish();
    emit picked(c);
}

QColor ScreenColorPicker::colorFromPortalResults(const QVariantMap &r) {
    const QVariant v = r.value(QStringLiteral("color"));
    double rgb[3] = {-1, -1, -1};
#ifdef LW_HAVE_DBUS
    if (v.canConvert<QDBusArgument>()) {
        const QDBusArgument a = v.value<QDBusArgument>();
        a.beginStructure();
        a >> rgb[0] >> rgb[1] >> rgb[2];
        a.endStructure();
    } else
#endif
    {
        const QVariantList l = v.toList();
        if (l.size() == 3) for (int i = 0; i < 3; ++i) rgb[i] = l[i].toDouble();
    }
    for (double x : rgb) if (x < 0 || x > 1.0001) return QColor();
    return QColor::fromRgbF(float(rgb[0]), float(rgb[1]), float(rgb[2]));
}

bool ScreenColorPicker::startPortal() {
#ifdef LW_HAVE_DBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    const QString token = QStringLiteral("lw%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    // subscribe before calling, so a fast reply is not lost
    bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), path, QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"), this, SLOT(portalResponse(uint,QVariantMap)));
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
                                                    QStringLiteral("org.freedesktop.portal.Screenshot"), QStringLiteral("PickColor"));
    QString parent;
    if (m_owner && m_owner->window()->windowHandle()) parent = QString();   // xdg-foreign handles need a toplevel export; "" is valid
    m << parent << QVariantMap{{QStringLiteral("handle_token"), token}};
    auto *w = new QDBusPendingCallWatcher(bus.asyncCall(m), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *c) {
        c->deleteLater();
        QDBusPendingReply<QDBusObjectPath> r = *c;
        if (r.isError()) cancel();
    });
    return true;
#else
    return false;
#endif
}

void ScreenColorPicker::portalResponse(uint response, const QVariantMap &results) {
    const QColor c = response == 0 ? colorFromPortalResults(results) : QColor();
    if (c.isValid()) { emit hovered(c); pick(c); }
    else cancel();
}

// ---------------------------------------------------------------- colorpick
namespace colorpick {

QToolButton *eyedropperButton(QWidget *parent) {
    auto *b = new QToolButton(parent);
    b->setObjectName(QStringLiteral("eyedropper"));
    b->setIcon(icons::tool(Tool::Eyedropper));
    b->setAutoRaise(true);
    b->setToolTip(QStringLiteral("Pick a colour from the screen (click to pick, Esc or right-click to cancel)"));
    return b;
}

void addEyedropper(QColorDialog *dlg) {
    if (!dlg || dlg->findChild<QToolButton *>(QStringLiteral("eyedropper"))) return;
    QToolButton *b = eyedropperButton(dlg);
    b->setText(QStringLiteral("Pick from Screen"));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // Qt's own "Pick Screen Color" button: replaced by ours (it cannot pick on Wayland)
    QPushButton *qtPick = nullptr;
    for (QPushButton *pb : dlg->findChildren<QPushButton *>())
        if (pb->text().remove(QLatin1Char('&')).contains(QLatin1String("Pick Screen"), Qt::CaseInsensitive)) qtPick = pb;
    bool placed = false;
    if (qtPick && qtPick->parentWidget() && qtPick->parentWidget()->layout()) {
        QLayout *l = qtPick->parentWidget()->layout();
        if (auto *box = qobject_cast<QBoxLayout *>(l)) {
            const int i = box->indexOf(qtPick);
            if (i >= 0) { box->insertWidget(i, b); placed = true; }
        }
        qtPick->hide();
    }
    if (!placed && dlg->layout()) {
        if (auto *box = qobject_cast<QBoxLayout *>(dlg->layout())) { box->insertWidget(0, b, 0, Qt::AlignLeft); placed = true; }
        else dlg->layout()->addWidget(b);
    }
    QObject::connect(b, &QToolButton::clicked, dlg, [dlg] {
        const QColor before = dlg->currentColor();
        ScreenColorPicker *p = ScreenColorPicker::start(dlg);
        auto keepAlpha = [dlg](QColor c) { c.setAlpha(dlg->currentColor().alpha()); return c; };
        QObject::connect(p, &ScreenColorPicker::hovered, dlg, [dlg, keepAlpha](const QColor &c) { dlg->setCurrentColor(keepAlpha(c)); });
        QObject::connect(p, &ScreenColorPicker::picked, dlg, [dlg, keepAlpha](const QColor &c) { dlg->setCurrentColor(keepAlpha(c)); });
        QObject::connect(p, &ScreenColorPicker::cancelled, dlg, [dlg, before] { dlg->setCurrentColor(before); });
    });
}

QColor getColor(const QColor &initial, QWidget *parent, const QString &title, bool alpha) {
    QColorDialog dlg(initial, parent);
    dlg.setObjectName(QStringLiteral("colorDialog"));
    dlg.setWindowTitle(title.isEmpty() ? QStringLiteral("Choose Colour") : title);
    dlg.setOption(QColorDialog::DontUseNativeDialog);
    dlg.setOption(QColorDialog::ShowAlphaChannel, alpha);
    addEyedropper(&dlg);
    if (dlg.exec() != QDialog::Accepted) return QColor();
    return dlg.selectedColor();
}

} // namespace colorpick

// ---------------------------------------------------------------- ColorPopup
namespace {
constexpr int kSw = 11, kCols = 18, kRows = 13;
}

QVector<QColor> ColorPopup::swatches() {
    // Fireworks' "Color Cubes": 216 web-safe colours in 12 rows of 18, then 18 greys
    QVector<QColor> v;
    v.reserve(kCols * kRows);
    for (int row = 0; row < 12; ++row)
        for (int col = 0; col < kCols; ++col) {
            const int r = (row / 6) * 3 + col / 6, g = col % 6, b = row % 6;
            v << QColor(r * 51, g * 51, b * 51);
        }
    for (int i = 0; i < kCols; ++i) { const int g = qRound(i * 255.0 / (kCols - 1)); v << QColor(g, g, g); }
    return v;
}

ColorPopup::ColorPopup(const QColor &current, bool alpha, QWidget *parent)
    : QFrame(parent, Qt::Popup), m_current(current), m_shown(current), m_alpha(alpha) {
    setObjectName(QStringLiteral("colorPopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameShape(QFrame::StyledPanel);
    setMouseTracking(true);
    const int top = 30;
    m_preview = QRect(4, 4, 34, 22);
    m_grid = QRect(4, top, kCols * kSw + 1, kRows * kSw + 1);
    const int alphaH = 22, alphaGap = 6;
    const int alphaTop = m_grid.bottom() + alphaGap;
    setFixedSize(m_grid.right() + 5, alphaTop + alphaH + 5);
    m_alphaLabel = new QLabel(QStringLiteral("Alpha:"), this);
    m_alphaLabel->setObjectName(QStringLiteral("colorPopupAlphaLabel"));
    m_alphaLabel->setGeometry(4, alphaTop, 48, alphaH);
    m_alphaSlider = new QSlider(Qt::Horizontal, this);
    m_alphaSlider->setObjectName(QStringLiteral("colorPopupAlpha"));
    m_alphaSlider->setRange(0, 100);
    m_alphaSlider->setPageStep(10);
    PopupSliderHub::adoptSlider(m_alphaSlider);
    m_alphaSlider->setGeometry(54, alphaTop, width() - 54 - 48, alphaH);
    m_alphaValue = new QLabel(this);
    m_alphaValue->setObjectName(QStringLiteral("colorPopupAlphaValue"));
    m_alphaValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_alphaValue->setGeometry(width() - 48, alphaTop, 44, alphaH);
    const int pct0 = qBound(0, qRound(current.alphaF() * 100.0), 100);
    m_alphaSlider->setValue(pct0);
    m_alphaValue->setText(QStringLiteral("%1 %").arg(pct0));
    m_alphaLabel->setBuddy(m_alphaSlider);
    m_alphaLabel->setEnabled(alpha);
    m_alphaSlider->setEnabled(alpha);
    m_alphaValue->setEnabled(alpha);
    if (!alpha) {
        m_alphaSlider->setToolTip(QStringLiteral("This colour has no alpha channel."));
        m_alphaLabel->setToolTip(m_alphaSlider->toolTip());
    } else {
        m_alphaSlider->setToolTip(QStringLiteral("Opacity of the colour, 0–100 %. Shift: fine control. Ctrl: snap to the Preferences increment."));
        m_alphaLabel->setToolTip(m_alphaSlider->toolTip());
    }
    connect(m_alphaSlider, &QSlider::valueChanged, this, [this](int v) { setAlphaPercent(v, true); });
    m_hex = new QLineEdit(this);
    m_hex->setObjectName(QStringLiteral("colorPopupHex"));
    m_hex->setGeometry(m_preview.right() + 5, 4, 76, 22);
    m_hex->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("#?[0-9A-Fa-f]{0,8}")), m_hex));
    m_hex->setToolTip(alpha ? QStringLiteral("#RRGGBB or #AARRGGBB") : QStringLiteral("#RRGGBB"));
    auto *drop = colorpick::eyedropperButton(this);
    drop->setGeometry(width() - 4 - 2 * 26, 3, 24, 24);
    // hand over to the picker before closing, so a pop-up stays open throughout (closing the
    // last pop-up first can deactivate the application, which closes every pop-up)
    connect(drop, &QToolButton::clicked, this, [this] { m_committed = true; emit eyedropperRequested(); close(); });
    auto *more = new QToolButton(this);
    more->setObjectName(QStringLiteral("colorPopupMore"));
    more->setText(QStringLiteral("…"));
    more->setToolTip(QStringLiteral("More colours…"));
    more->setAutoRaise(true);
    more->setGeometry(width() - 4 - 24, 3, 24, 24);
    connect(more, &QToolButton::clicked, this, [this] { m_committed = true; close(); emit dialogRequested(); });
    connect(m_hex, &QLineEdit::returnPressed, this, [this] {
        QString t = m_hex->text().trimmed();
        if (!t.startsWith(QLatin1Char('#'))) t.prepend(QLatin1Char('#'));
        QColor c(t);
        if (!c.isValid() || (t.size() != 7 && t.size() != 9 && t.size() != 4)) return;
        if (!m_alpha) c.setAlpha(255);
        else if (t.size() != 9 && m_current.alpha() > 0) c.setAlpha(m_current.alpha());
        m_committed = true;
        close();
        emit picked(c);
    });
    showColor(current);
}

void ColorPopup::addTransparentChoice() {
    auto *t = new QToolButton(this);
    t->setObjectName(QStringLiteral("colorPopupTransparent"));
    t->setToolTip(QStringLiteral("Transparent"));
    t->setAutoRaise(true);
    QPixmap pm(16, 16);
    pm.fill(Qt::white);
    {
        QPainter p(&pm);
        p.setPen(QColor(0x40, 0x40, 0x40));
        p.drawRect(0, 0, 15, 15);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0xD0, 0x20, 0x20), 2));
        p.drawLine(QPointF(2, 14), QPointF(14, 2));
    }
    t->setIcon(QIcon(pm));
    t->setGeometry(width() - 4 - 3 * 26, 3, 24, 24);
    connect(t, &QToolButton::clicked, this, [this] { m_committed = true; close(); emit picked(QColor(0, 0, 0, 0)); });
}


QRect ColorPopup::swatchRect(int i) const {
    return QRect(m_grid.left() + (i % kCols) * kSw, m_grid.top() + (i / kCols) * kSw, kSw + 1, kSw + 1);
}

int ColorPopup::swatchAt(const QPoint &p) const {
    if (!m_grid.contains(p)) return -1;
    const int c = (p.x() - m_grid.left()) / kSw, r = (p.y() - m_grid.top()) / kSw;
    return (c < kCols && r < kRows) ? r * kCols + c : -1;
}

void ColorPopup::showColor(const QColor &c) {
    m_shown = c;
    if (m_hex && !m_hex->hasFocus())
        m_hex->setText(c.isValid() ? (m_alpha && c.alpha() < 255 ? c.name(QColor::HexArgb) : c.name(QColor::HexRgb)).toUpper() : QString());
    if (m_alphaSlider && m_alpha) {
        const int pct = qBound(0, qRound(c.alphaF() * 100.0), 100);
        if (m_alphaSlider->value() != pct) {
            QSignalBlocker b(m_alphaSlider);
            m_alphaSlider->setValue(pct);
        }
        if (m_alphaValue) m_alphaValue->setText(QStringLiteral("%1 %").arg(pct));
    }
    update();
}

void ColorPopup::setAlphaPercent(int pct, bool preview)
{
    if (!m_alpha) return;
    pct = qBound(0, pct, 100);
    m_current.setAlphaF(pct / 100.0);
    m_alphaEdited = true;
    if (m_hover >= 0) {
        QColor c = swatches().value(m_hover);
        c.setAlpha(m_current.alpha());
        showColor(c);
        if (preview) emit hovered(c);
    } else {
        showColor(m_current);
        if (preview) emit hovered(m_current);
    }
}

void ColorPopup::hideEvent(QHideEvent *e)
{
    QFrame::hideEvent(e);
    if (!m_committed && m_alpha && m_alphaEdited) {
        m_committed = true;
        emit picked(m_current);
    }
}

void ColorPopup::paintEvent(QPaintEvent *e) {
    QFrame::paintEvent(e);
    QPainter p(this);
    // the preview swatch: checkerboard under the colour, so alpha shows
    for (int y = m_preview.top(); y <= m_preview.bottom(); y += 4)
        for (int x = m_preview.left(); x <= m_preview.right(); x += 4)
            p.fillRect(QRect(x, y, 4, 4).intersected(m_preview), ((x - m_preview.left()) / 4 + (y - m_preview.top()) / 4) % 2 ? QColor(0xCC, 0xCC, 0xCC) : QColor(Qt::white));
    p.fillRect(m_preview, m_shown);
    p.setPen(QColor(0x40, 0x40, 0x40));
    p.drawRect(m_preview.adjusted(0, 0, -1, -1));
    const QVector<QColor> sw = swatches();
    for (int i = 0; i < sw.size(); ++i) {
        const QRect r = swatchRect(i);
        p.fillRect(r, sw[i]);
        p.setPen(Qt::black);
        p.drawRect(r.adjusted(0, 0, -1, -1));
    }
    if (m_hover >= 0) {
        p.setPen(Qt::white);
        p.drawRect(swatchRect(m_hover).adjusted(0, 0, -1, -1));
    }
}

void ColorPopup::mouseMoveEvent(QMouseEvent *e) {
    const int i = swatchAt(e->position().toPoint());
    if (i == m_hover) return;
    m_hover = i;
    QColor c = i >= 0 ? swatches()[i] : m_current;
    if (i >= 0) c.setAlpha(m_alpha && m_current.alpha() > 0 ? m_current.alpha() : 255);   // swatches keep the alpha
    showColor(c);
    emit hovered(c);
}

void ColorPopup::leaveEvent(QEvent *) {
    if (m_hover < 0) return;
    m_hover = -1;
    showColor(m_current);
    emit hovered(m_current);
}

void ColorPopup::mousePressEvent(QMouseEvent *e) {
    if (!rect().contains(e->position().toPoint())) { QFrame::mousePressEvent(e); return; }   // outside: close
    e->accept();
}

void ColorPopup::mouseReleaseEvent(QMouseEvent *e) {
    const int i = swatchAt(e->position().toPoint());
    if (e->button() != Qt::LeftButton || i < 0) return;
    QColor c = swatches()[i];
    c.setAlpha(m_alpha && m_current.alpha() > 0 ? m_current.alpha() : 255);
    m_committed = true;
    close();
    emit picked(c);
}
