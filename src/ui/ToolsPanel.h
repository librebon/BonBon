#pragma once
#include "AppState.h"
#include <QHash>
#include <QWidget>

#include <QToolButton>

class QMenu;
class QTimer;
class QActionGroup;
class ColorButton;

// A Tools panel button for a group of tools (Fireworks flyout). The flyout menu opens when you
// click the dark corner triangle, press and hold the button (Preferences, default 150 ms), press
// and drag more than kDragOpenPx, or right-click it. When it opened from a held press you can
// drag onto an item and release to pick it in one gesture; clicking items works as usual too.
// Any click on the button selects the tool currently shown on it (including a press that opens
// the flyout, then a release outside it, or Escape). Picking a flyout item still switches to it.
class FlyoutToolButton : public QToolButton {
    Q_OBJECT
public:
    FlyoutToolButton(const QList<Tool> &tools, QWidget *parent = nullptr);
    QMenu *flyout() const { return m_menu; }
    Tool currentTool() const { return m_current; }
    void setCurrentTool(Tool t);         // icon + tooltip; does not change the active tool
    void setChevronOnHover(bool on) { m_chevronOnHover = on; update(); }
    bool chevronOnHover() const { return m_chevronOnHover; }
    QRect arrowRect() const;             // the clickable corner area
    void showFlyout();
    void activateShownTool();                   // appState().setTool(currentTool())
    static constexpr int kDefaultHoldMs = 150;  // Prefs::flyoutHoldMs default
    static constexpr int kDragOpenPx = 3;       // press + move further than this opens the flyout
    static int holdMs();                        // the current (preference) hold delay
protected:
    bool eventFilter(QObject *o, QEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    QList<Tool> m_tools;
    Tool m_current;
    QMenu *m_menu;
    QActionGroup *m_actions;
    QTimer *m_hold;
    bool m_swallowRelease = false;
    bool m_pressDrag = false;   // flyout opened by the still-held left press: release over an item picks it
    QPoint m_pressPos;
    bool m_chevronOnHover = false;
    Tool m_toolAtShow{};  // app tool when this flyout opened, for dismiss logic
};

// Fireworks-style Tools panel: labelled sections with a two column grid of tool buttons.
class ToolsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolsPanel(QWidget *parent = nullptr);
    void refreshShortcutLabels(); // re-read tips and flyout menus from the shortcut registry
    void syncWebFeaturePref();
private:
    void syncColors();
    void syncToolChecks(Tool t); // exactly one tool button checked, none else down
    QHash<int, QToolButton *> m_buttons;
    QToolButton *m_webHide = nullptr;
    QToolButton *m_webShow = nullptr;
    QWidget *m_webSection = nullptr;
    ColorButton *m_stroke, *m_fill;
    QToolButton *m_noStroke, *m_noFill;
};
