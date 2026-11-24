#include "Theme.h"
#include "ToolsPanel.h"
#include "Shortcuts.h"
#include <QKeySequence>
#include "Dialogs.h"
#include "Icons.h"
#include <QActionGroup>
#include <QFont>
#include <QButtonGroup>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QPainter>
#include <QTimer>
#include <QGridLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QMenu>
#include <QApplication>
#include <QSignalBlocker>
#include <QCursor>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

static QString toolKeyText(Tool t) {
    // First shortcut only (tooltip / flyout). Empty when unbound — never fall back to the
    // hardcoded letter after the registry is live, or removed shortcuts would still show.
    return shortcuts::Manager::get().displayShortcutText(toolActionId(t));
}

static QString flyoutTip(Tool t) {
    QString tip = toolName(t);
    const QString k = toolKeyText(t);
    if (!k.isEmpty()) tip += QStringLiteral(" (%1)").arg(k);
    return tip + QStringLiteral(" - click the arrow or hold for more tools");
}

FlyoutToolButton::FlyoutToolButton(const QList<Tool> &tools, QWidget *parent)
    : QToolButton(parent), m_tools(tools), m_current(tools.first()) {
    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("flyout_") + toolName(tools.first()).remove(QLatin1Char(' ')));
    m_actions = new QActionGroup(m_menu);
    m_actions->setExclusive(true);
    for (Tool o : tools) {
        QString text = toolName(o);
        const QString k = toolKeyText(o);
        if (!k.isEmpty()) text += QStringLiteral("\t") + k;
        QAction *a = m_menu->addAction(icons::tool(o), text);
        a->setCheckable(true);
        a->setData(int(o));
        a->setObjectName(QStringLiteral("flyoutItem_") + toolName(o).remove(QLatin1Char(' ')));
        m_actions->addAction(a);
        connect(a, &QAction::triggered, this, [this, o] { setCurrentTool(o); appState().setTool(o); });
    }
    connect(m_menu, &QMenu::aboutToShow, this, [this] {
        for (QAction *a : m_actions->actions()) {
            const bool cur = Tool(a->data().toInt()) == m_current;
            a->setChecked(cur);
            QFont f = a->font();
            f.setBold(cur);            // the style's check mark is subtle next to an icon
            a->setFont(f);
        }
    });
    m_hold = new QTimer(this);
    m_hold->setSingleShot(true);
    connect(m_hold, &QTimer::timeout, this, [this] {
        if (!isDown()) return;
        setDown(false);              // the press becomes "open the flyout", not a click
        m_swallowRelease = true;
        m_pressDrag = true;
        showFlyout();
    });
    m_menu->installEventFilter(this);
    connect(m_menu, &QMenu::aboutToHide, this, [this] {
        m_pressDrag = false;
        setDown(false);
        // If the tool already changed to something this button does not show
        // (another tool clicked, a shortcut, a programmatic setTool), leave it.
        // Otherwise Escape / a click outside keeps the tool shown on this button.
        const Tool active = appState().tool();
        if (!m_tools.contains(active) && active != m_toolAtShow)
            return;
        if (QWidget *hit = QApplication::widgetAt(QCursor::pos())) {
            QToolButton *tb = qobject_cast<QToolButton *>(hit);
            if (!tb && hit->parentWidget())
                tb = qobject_cast<QToolButton *>(hit->parentWidget());
            if (tb && tb != this && tb->property("lwTool").isValid())
                return;
        }
        activateShownTool();
    });
    setCurrentTool(m_current);
}

void FlyoutToolButton::activateShownTool()
{
    appState().setTool(m_current);
}

void FlyoutToolButton::setCurrentTool(Tool t) {
    if (!m_tools.contains(t)) return;
    m_current = t;
    setProperty("lwTool", int(t));
    setIcon(icons::tool(t));
    setToolTip(flyoutTip(t));
}

int FlyoutToolButton::holdMs() { return qBound(50, appState().prefs.flyoutHoldMs, 2000); }

QRect FlyoutToolButton::arrowRect() const { return QRect(width() - 9, height() - 9, 9, 9); }

void FlyoutToolButton::showFlyout() {
    m_hold->stop();
    if (m_menu->isVisible()) return;
    m_toolAtShow = appState().tool();
    m_menu->popup(mapToGlobal(QPoint(width(), 0)));   // to the right of the button, as in Fireworks
}

void FlyoutToolButton::paintEvent(QPaintEvent *e) {
    QToolButton::paintEvent(e);
    // the flyout indicator: a solid dark triangle in the bottom-right corner
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    if (m_chevronOnHover && !underMouse()) return;
    p.setBrush(isEnabled() ? theme::color("arrow") : theme::color("disabled"));
    const int r = width() - 2, b = height() - 2;
    p.drawPolygon(QPolygon({QPoint(r, b - 5), QPoint(r, b), QPoint(r - 5, b)}));
}

void FlyoutToolButton::enterEvent(QEnterEvent *e)
{
    QToolButton::enterEvent(e);
    if (m_chevronOnHover) update();
}

void FlyoutToolButton::leaveEvent(QEvent *e)
{
    QToolButton::leaveEvent(e);
    if (m_chevronOnHover) update();
}

void FlyoutToolButton::mousePressEvent(QMouseEvent *e) {
    m_swallowRelease = false;
    if (e->button() == Qt::RightButton) {   // right-click (also handled via contextMenuEvent)
        m_swallowRelease = true;
        e->accept();
        activateShownTool();
        showFlyout();
        return;
    }
    if (e->button() == Qt::LeftButton && arrowRect().contains(e->position().toPoint())) {
        // corner arrow: select the shown tool and open the flyout at once
        m_swallowRelease = true;
        e->accept();
        activateShownTool();
        showFlyout();
        return;
    }
    QToolButton::mousePressEvent(e);
    if (e->button() == Qt::LeftButton) {
        m_pressPos = e->position().toPoint();
        activateShownTool();
        m_hold->start(holdMs());
    }
}

void FlyoutToolButton::mouseMoveEvent(QMouseEvent *e) {
    // press and drag (on or off the button) opens the flyout at once, like Fireworks
    if ((e->buttons() & Qt::LeftButton) && m_hold->isActive() && !m_menu->isVisible()
        && (e->position().toPoint() - m_pressPos).manhattanLength() > kDragOpenPx) {
        m_hold->stop();
        setDown(false);
        m_swallowRelease = true;
        m_pressDrag = true;
        showFlyout();
        e->accept();
        return;
    }
    QToolButton::mouseMoveEvent(e);
}

bool FlyoutToolButton::eventFilter(QObject *o, QEvent *e) {
    // press-drag-release: while the opening press is still held, releasing over an item picks it.
    // Releasing anywhere else (e.g. still over the button) leaves the menu open for a normal click.
    if (o == m_menu && m_pressDrag) {
        if (e->type() == QEvent::MouseButtonPress) m_pressDrag = false;   // a fresh click: QMenu handles it
        else if (e->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return false;
            m_pressDrag = false;
            m_swallowRelease = false;
            setDown(false);
            QAction *a = m_menu->actionAt(me->position().toPoint());
            if (a && a->isEnabled() && m_menu->rect().contains(me->position().toPoint())) {
                m_menu->hide();
                a->trigger();
            }
            return true;
        }
    }
    return QToolButton::eventFilter(o, e);
}

void FlyoutToolButton::mouseReleaseEvent(QMouseEvent *e) {
    m_hold->stop();
    m_pressDrag = m_pressDrag && m_menu->isVisible();
    if (m_swallowRelease) {
        m_swallowRelease = false;
        setDown(false);
        e->accept();
        return;
    }
    QToolButton::mouseReleaseEvent(e);
}

void FlyoutToolButton::contextMenuEvent(QContextMenuEvent *e) {
    e->accept();
    activateShownTool();
    showFlyout();
}

ToolsPanel::ToolsPanel(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ToolsPanel"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(3, 3, 3, 3);
    v->setSpacing(1);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);

    auto section = [&](const QString &title, QVBoxLayout *into = nullptr) {
        auto *lay = into ? into : v;
        auto *l = new QLabel(title);
        l->setObjectName(QStringLiteral("ToolSection"));
        l->setAlignment(Qt::AlignCenter);
        // Headers must not grow if a parent layout has leftover height.
        l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        lay->addWidget(l);
    };
    auto grid = [&](std::initializer_list<Tool> tools, QVBoxLayout *into = nullptr) {
        auto *lay = into ? into : v;
        auto *g = new QGridLayout;
        g->setSpacing(1);
        g->setContentsMargins(2, 1, 2, 3);
        int i = 0;
        for (Tool t : tools) {
            const QList<Tool> grp = toolGroup(t);
            QToolButton *b = grp.size() > 1 ? new FlyoutToolButton(grp) : new QToolButton;
            if (grp.size() > 1) for (Tool o : grp) m_buttons[int(o)] = b;
            if (grp.size() == 1) b->setIcon(icons::tool(t));
            b->setIconSize(QSize(20, 20));
            b->setFixedSize(28, 26);
            b->setCheckable(true);
            b->setAutoRaise(true);
            if (grp.size() == 1) {
                QString tip = toolName(t);
                const QString k = toolKeyText(t);
                if (!k.isEmpty()) tip += QStringLiteral(" (%1)").arg(k);
                b->setToolTip(tip);
            }
            b->setObjectName(QStringLiteral("tool_") + toolName(t).remove(QLatin1Char(' ')));
            b->setProperty("lwTool", int(t));
            group->addButton(b, int(t));
            m_buttons[int(t)] = b;
            g->addWidget(b, i / 2, i % 2);
            ++i;
        }
        lay->addLayout(g);
    };

    section(QStringLiteral("Select"));
    grid({Tool::Pointer, Tool::Subselect, Tool::Scale, Tool::Crop});
    section(QStringLiteral("Bitmap"));
    grid({Tool::Marquee, Tool::Lasso, Tool::MagicWand, Tool::Brush, Tool::Pencil, Tool::Eraser,
          Tool::Blur, Tool::RubberStamp, Tool::Eyedropper, Tool::PaintBucket});
    section(QStringLiteral("Vector"));
    grid({Tool::Line, Tool::Pen, Tool::Rect, Tool::Text, Tool::Knife, Tool::Freeform});
    m_webSection = new QWidget;
    m_webSection->setObjectName(QStringLiteral("toolSection_Web"));
    // Preferred/Preferred would absorb leftover dock height and stretch the "Web"
    // header (43px vs 19px for Select/Bitmap/Vector/Colors/View). Maximum keeps
    // the wrapper at its size hint, matching 0.3.14 / 0.3.21 where the header
    // was a direct child of the main column layout.
    m_webSection->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *webLay = new QVBoxLayout(m_webSection);
    webLay->setContentsMargins(0, 0, 0, 0);
    webLay->setSpacing(1);
    section(QStringLiteral("Web"), webLay);
    {
        grid({Tool::Slice, Tool::Hotspot}, webLay);
        auto *row = new QHBoxLayout;
        row->setContentsMargins(2, 0, 2, 3);
        row->setSpacing(1);
        auto *hide = new QToolButton;
        hide->setIcon(icons::named(QStringLiteral("webhide")));
        m_webHide = hide;
        hide->setToolTip(QStringLiteral("Hide slices and hotspots"));
        hide->setFixedSize(28, 22);
        hide->setAutoRaise(true);
        auto *show = new QToolButton;
        show->setIcon(icons::named(QStringLiteral("web")));
        m_webShow = show;
        show->setToolTip(QStringLiteral("Show slices and hotspots"));
        show->setFixedSize(28, 22);
        show->setAutoRaise(true);
        connect(hide, &QToolButton::clicked, this, [] { appState().webLayerVisible = false; appState().notifyOptions(); });
        connect(show, &QToolButton::clicked, this, [] { appState().webLayerVisible = true; appState().notifyOptions(); });
        row->addWidget(hide);
        row->addWidget(show);
        row->addStretch();
        webLay->addLayout(row);
    }
    v->addWidget(m_webSection);
    section(QStringLiteral("Colors"));
    {
        auto *g = new QGridLayout;
        g->setContentsMargins(2, 1, 2, 3);
        g->setSpacing(2);
        m_stroke = new ColorButton(this, true);
        m_stroke->setFixedSize(42, 22);
        m_stroke->setIconHint(icons::tool(Tool::Pencil));
        m_stroke->setToolTip(QStringLiteral("Stroke colour"));
        m_fill = new ColorButton(this, true);
        m_fill->setFixedSize(42, 22);
        m_fill->setIconHint(icons::tool(Tool::PaintBucket));
        m_fill->setToolTip(QStringLiteral("Fill colour"));
        m_noStroke = new QToolButton; m_noStroke->setCheckable(true); m_noStroke->setText(QStringLiteral("/"));
        m_noStroke->setToolTip(QStringLiteral("No stroke")); m_noStroke->setFixedSize(14, 22);
        m_noFill = new QToolButton; m_noFill->setCheckable(true); m_noFill->setText(QStringLiteral("/"));
        m_noFill->setToolTip(QStringLiteral("No fill")); m_noFill->setFixedSize(14, 22);
        g->addWidget(m_stroke, 0, 0);
        g->addWidget(m_noStroke, 0, 1);
        g->addWidget(m_fill, 1, 0);
        g->addWidget(m_noFill, 1, 1);
        auto *row = new QHBoxLayout;
        row->setSpacing(1);
        auto *def = new QToolButton;
        def->setObjectName(QStringLiteral("btnDefaultColors"));
        def->setIcon(icons::named(QStringLiteral("default")));
        {
            QString tip = QStringLiteral("Default stroke/fill colours");
            const QString k = [&] {
                const auto seqs = shortcuts::Manager::get().sequences(QStringLiteral("colors.default"));
                if (!seqs.isEmpty() && !seqs.first().isEmpty()) return seqs.first().toString(QKeySequence::NativeText);
                return QStringLiteral("D");
            }();
            if (!k.isEmpty()) tip += QStringLiteral(" (%1)").arg(k);
            def->setToolTip(tip);
        }
        def->setAutoRaise(true);
        def->setFixedSize(22, 20);
        auto *swap = new QToolButton;
        swap->setObjectName(QStringLiteral("btnSwapColors"));
        swap->setIcon(icons::named(QStringLiteral("swap")));
        {
            QString tip = QStringLiteral("Swap stroke/fill colours");
            const QString k = [&] {
                const auto seqs = shortcuts::Manager::get().sequences(QStringLiteral("colors.swap"));
                if (!seqs.isEmpty() && !seqs.first().isEmpty()) return seqs.first().toString(QKeySequence::NativeText);
                return QStringLiteral("X");
            }();
            if (!k.isEmpty()) tip += QStringLiteral(" (%1)").arg(k);
            swap->setToolTip(tip);
        }
        swap->setAutoRaise(true);
        swap->setFixedSize(22, 20);
        row->addWidget(def);
        row->addWidget(swap);
        row->addStretch();
        g->addLayout(row, 2, 0, 1, 2);
        v->addLayout(g);
        connect(m_stroke, &ColorButton::colorPicked, this, [](const QColor &c) { appState().strokeOn = true; appState().setStroke(c); });
        connect(m_fill, &ColorButton::colorPicked, this, [](const QColor &c) { appState().fillOn = true; appState().setFill(c); });
        connect(m_noStroke, &QToolButton::toggled, this, [](bool on) { if (appState().strokeOn == !on) return; appState().strokeOn = !on; emit appState().colorsChanged(); });
        connect(m_noFill, &QToolButton::toggled, this, [](bool on) { if (appState().fillOn == !on) return; appState().fillOn = !on; emit appState().colorsChanged(); });
        connect(def, &QToolButton::clicked, this, [] { appState().strokeOn = appState().fillOn = true; appState().defaultColors(); });
        connect(swap, &QToolButton::clicked, this, [] { appState().swapColors(); });
    }
    section(QStringLiteral("View"));
    grid({Tool::Hand, Tool::Zoom});
    v->addStretch();

    auto selectFromButton = [](QAbstractButton *b) {
        // group buttons remember the tool last chosen from their flyout
        appState().setTool(Tool(b->property("lwTool").isValid() ? b->property("lwTool").toInt() : 0));
    };
    connect(group, &QButtonGroup::buttonPressed, this, selectFromButton);
    connect(group, &QButtonGroup::buttonClicked, this, selectFromButton);
    for (QAbstractButton *b : group->buttons())
        if (!b->property("lwTool").isValid()) b->setProperty("lwTool", group->id(b));
    connect(&appState(), &AppState::toolChanged, this, [this](Tool t) {
        syncToolChecks(t);
    });
    connect(&appState(), &AppState::colorsChanged, this, &ToolsPanel::syncColors);
    connect(&appState(), &AppState::prefsChanged, this, [this] {
        syncWebFeaturePref();
    });
    syncWebFeaturePref();
    syncToolChecks(appState().tool());
    syncColors();
}

void ToolsPanel::syncToolChecks(Tool t)
{
    QToolButton *want = m_buttons.value(int(t));
    if (auto *fb = qobject_cast<FlyoutToolButton *>(want))
        fb->setCurrentTool(t);
    // m_buttons maps every flyout member onto the same widget; unique-ify.
    QList<QToolButton *> all;
    for (QToolButton *b : m_buttons) {
        if (b && !all.contains(b)) all.append(b);
    }
    for (QToolButton *b : all) {
        const bool on = (b == want);
        if (b->isChecked() != on) {
            QSignalBlocker block(b);
            b->setChecked(on);
        }
        if (!on && b->isDown())
            b->setDown(false);
    }
}

void ToolsPanel::syncColors() {
    m_stroke->setColor(appState().stroke);
    m_fill->setColor(appState().fill);
    m_stroke->setNone(!appState().strokeOn);
    m_fill->setNone(!appState().fillOn);
    QSignalBlocker a(m_noStroke), b(m_noFill);
    m_noStroke->setChecked(!appState().strokeOn);
    m_noFill->setChecked(!appState().fillOn);
}

void ToolsPanel::refreshShortcutLabels() {
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        QToolButton *b = it.value();
        if (!b) continue;
        if (auto *fb = qobject_cast<FlyoutToolButton *>(b)) {
            fb->setCurrentTool(fb->currentTool()); // refreshes tip via flyoutTip / toolKeyText
            if (QMenu *m = fb->flyout()) {
                for (QAction *a : m->actions()) {
                    if (!a->data().isValid()) continue;
                    const Tool o = Tool(a->data().toInt());
                    QString text = toolName(o);
                    const QString k = toolKeyText(o);
                    if (!k.isEmpty()) text += QStringLiteral("\t") + k;
                    a->setText(text);
                }
            }
            continue;
        }
        const Tool t = Tool(b->property("lwTool").toInt());
        QString tip = toolName(t);
        const QString k = toolKeyText(t);
        if (!k.isEmpty()) tip += QStringLiteral(" (%1)").arg(k);
        b->setToolTip(tip);
    }
    if (auto *swap = findChild<QToolButton *>(QStringLiteral("btnSwapColors"))) {
        QString tip = QStringLiteral("Swap stroke/fill colours");
        const QList<QKeySequence> seqs = shortcuts::Manager::get().sequences(QStringLiteral("colors.swap"));
        if (!seqs.isEmpty() && !seqs.first().isEmpty())
            tip += QStringLiteral(" (%1)").arg(seqs.first().toString(QKeySequence::NativeText));
        swap->setToolTip(tip);
    }
    if (auto *def = findChild<QToolButton *>(QStringLiteral("btnDefaultColors"))) {
        QString tip = QStringLiteral("Default stroke/fill colours");
        const QList<QKeySequence> seqs = shortcuts::Manager::get().sequences(QStringLiteral("colors.default"));
        if (!seqs.isEmpty() && !seqs.first().isEmpty())
            tip += QStringLiteral(" (%1)").arg(seqs.first().toString(QKeySequence::NativeText));
        def->setToolTip(tip);
    }
}

void ToolsPanel::syncWebFeaturePref()
{
    const bool on = appState().prefs.enableWebFeatures;
    if (m_webSection)
        m_webSection->setVisible(on);
}
