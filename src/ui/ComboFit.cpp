#include "ComboFit.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QMainWindow>
#include <QDialog>
#include <QDockWidget>
#include <QEvent>
#include <QFontComboBox>
#include <QFontMetrics>
#include <QFrame>
#include <QPointer>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTimer>
#include <QWidget>

namespace combofit {
namespace {

const char kFitted[] = "_lwComboFit";
const char kFileType[] = "_lwFilterPopup";

int closedCapFor(const QComboBox *combo)
{
    if (!combo) return kDefaultCap;
    for (const QWidget *w = combo; w; w = w->parentWidget()) {
        if (w->inherits("PropertyInspector")
            || w->objectName() == QLatin1String("PropertyInspector")
            || w->objectName() == QLatin1String("toolOptions")
            || qobject_cast<const QDockWidget *>(w))
            return kInspectorCap;
    }
    return kDefaultCap;
}

int textWidth(const QComboBox *combo, const QString &text)
{
    const QFontMetrics fm(combo->font());
    return fm.horizontalAdvance(text);
}

// Don't walk huge models (QFontComboBox can list thousands of families)
// on the closed-combo / theme-switch path.
const int kWalkCap = 48;

int widestItem(const QComboBox *combo, int limit = -1)
{
    int w = 0;
    const int n = combo->count();
    const int upto = (limit < 0) ? n : qMin(n, limit);
    for (int i = 0; i < upto; ++i)
        w = qMax(w, textWidth(combo, combo->itemText(i)));
    return w;
}

bool isHugeModel(const QComboBox *combo)
{
    return qobject_cast<const QFontComboBox *>(combo) || combo->count() > kWalkCap;
}

int chromeWidth(const QComboBox *combo)
{
    QStyleOptionComboBox opt;
    opt.initFrom(combo);
    opt.editable = combo->isEditable();
    opt.frame = combo->hasFrame();
    const int frame = combo->style()->pixelMetric(QStyle::PM_ComboBoxFrameWidth, &opt, combo) * 2
                      + combo->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, combo) * 2;
    const QRect arrow = combo->style()->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxArrow, combo);
    int arrowW = arrow.width();
    if (arrowW <= 0)
        arrowW = combo->style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &opt, combo) + 8;
    int iconW = 0;
    if (combo->isEditable() || !combo->itemIcon(combo->currentIndex()).isNull())
        iconW = combo->iconSize().width() + 6;
    // QFontComboBox draws a font-sample preview
    if (qobject_cast<const QFontComboBox *>(combo))
        iconW = qMax(iconW, combo->iconSize().width() + 8);
    return qMax(24, arrowW + frame + iconW + 12);
}

void applyCurrentTooltip(QComboBox *combo)
{
    const QString cur = combo->currentText();
    const int avail = combo->width() - chromeWidth(combo);
    const bool elided = avail > 0 && textWidth(combo, cur) > avail;
    if (elided)
        combo->setToolTip(cur);
    else if (combo->toolTip() == cur)
        combo->setToolTip(QString());
}

void applyItemTooltips(QComboBox *combo, bool allItems)
{
    if (!combo) return;
    // Only write a tooltip when it is missing or stale. setItemData emits
    // dataChanged, and watchModel() would recurse forever if we set a tip
    // that already equals the item text. Skip huge models: walking every
    // font family on a theme switch was taking ~2 s.
    if (allItems && !isHugeModel(combo)) {
        for (int i = 0; i < combo->count(); ++i) {
            const QString text = combo->itemText(i);
            const QString tip = combo->itemData(i, Qt::ToolTipRole).toString();
            if (tip != text)
                combo->setItemData(i, text, Qt::ToolTipRole);
        }
    }
    applyCurrentTooltip(combo);
}

int popupCap(const QComboBox *combo)
{
    int cap = 720;
    if (QScreen *sc = combo->screen())
        cap = qMin(cap, qRound(double(sc->availableGeometry().width()) * 0.60));
    if (QWidget *win = combo->window())
        cap = qMax(cap, qMin(win->width(), cap));
    return qMax(closedCapFor(combo), cap);
}

void sizePopup(QComboBox *combo)
{
    if (!combo) return;
    QAbstractItemView *view = combo->view();
    if (!view) return;
    view->setTextElideMode(Qt::ElideMiddle);
    applyItemTooltips(combo, true);
    const int cap = popupCap(combo);
    int widest = isHugeModel(combo)
                     ? qMax(textWidth(combo, combo->currentText()), widestItem(combo, kWalkCap))
                     : widestItem(combo);
    QScrollBar *vsb = view->verticalScrollBar();
    if (vsb) vsb->ensurePolished();
    view->ensurePolished();
    int scroll = 20;
    if (vsb) {
        scroll = qMax(vsb->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, vsb),
                      qMax(vsb->sizeHint().width(), vsb->isVisible() ? vsb->width() : 0));
    }
    const QMargins m = view->contentsMargins();
    const int fromItems = widest + scroll + m.left() + m.right() + 16;
    const int lo = combo->width();
    const int viewWant = qBound(lo, fromItems, cap);
    view->setMinimumWidth(0);
    view->setMaximumWidth(cap);
    if (QWidget *container = view->window()) {
        if (container != combo->window()) {
            container->setMaximumWidth(cap);
            if (container->width() < viewWant || container->width() > cap)
                container->resize(viewWant, container->height());
        }
    }
}

class ComboFitFilter final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override {
        const auto t = e->type();
        if (t == QEvent::Polish || t == QEvent::Show || t == QEvent::StyleChange
            || t == QEvent::FontChange) {
            if (auto *c = qobject_cast<QComboBox *>(o)) {
                if (!isFileTypeCombo(c))
                    apply(c);
            }
        }
        if (t == QEvent::Show) {
            if (auto *view = qobject_cast<QAbstractItemView *>(o)) {
                QObject *p = view->parent();
                while (p && !qobject_cast<QComboBox *>(p)) p = p->parent();
                if (auto *c = qobject_cast<QComboBox *>(p)) {
                    if (!isFileTypeCombo(c) && c->view() == view)
                        sizePopup(c);
                }
            } else if (auto *w = qobject_cast<QWidget *>(o)) {
                QWidget *win = w->window();
                if (win && win != w && win != w->parentWidget()) {
                    if (QAbstractItemView *view = win->findChild<QAbstractItemView *>()) {
                        QObject *p = view;
                        while (p && !qobject_cast<QComboBox *>(p)) p = p->parent();
                        if (auto *c = qobject_cast<QComboBox *>(p)) {
                            if (!isFileTypeCombo(c) && c->view() == view)
                                sizePopup(c);
                        }
                    }
                }
            }
        }
        return false;
    }
};

QPointer<ComboFitFilter> g_filter;

void watchModel(QComboBox *combo)
{
    if (!combo || combo->property("_lwComboFitWatch").toBool()) return;
    combo->setProperty("_lwComboFitWatch", true);
    auto run = [combo] { if (combo) apply(combo); };
    QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), combo, run);
    if (QAbstractItemModel *model = combo->model()) {
        QObject::connect(model, &QAbstractItemModel::modelReset, combo, run);
        QObject::connect(model, &QAbstractItemModel::rowsInserted, combo, run);
        QObject::connect(model, &QAbstractItemModel::dataChanged, combo, run);
    }
}

} // namespace

bool isFileTypeCombo(const QComboBox *combo)
{
    if (!combo) return false;
    if (combo->property(kFileType).toBool()) return true;
    if (combo->objectName() == QLatin1String("fileTypeCombo")) return true;
    return false;
}

void apply(QComboBox *combo)
{
    if (!combo || isFileTypeCombo(combo)) return;
    if (combo->property("_lwComboFitBusy").toBool()) return;
    combo->setProperty("_lwComboFitBusy", true);
    const auto clearBusy = qScopeGuard([combo] { combo->setProperty("_lwComboFitBusy", false); });
    install();
    watchModel(combo);
    if (QAbstractItemView *view = combo->view())
        view->setTextElideMode(Qt::ElideMiddle);

    if (!combo->property("_lwComboFitOrigMcl").isValid())
        combo->setProperty("_lwComboFitOrigMcl", combo->minimumContentsLength());
    if (!combo->property("_lwComboFitOrigMax").isValid())
        combo->setProperty("_lwComboFitOrigMax", combo->maximumWidth());

    int cap = closedCapFor(combo);
    const int origMax = combo->property("_lwComboFitOrigMax").toInt();
    if (origMax > 0 && origMax < QWIDGETSIZE_MAX)
        cap = qMin(cap, origMax);
    const int origMcl = combo->property("_lwComboFitOrigMcl").toInt();
    const int wantText = isHugeModel(combo)
                              ? textWidth(combo, combo->currentText())
                              : qMax(textWidth(combo, combo->currentText()), widestItem(combo));
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMaximumWidth(cap);
    if (combo->minimumWidth() > cap)
        combo->setMinimumWidth(0);

    // Grow the closed width only on roomy, already-parented surfaces.
    // Inspector and dock combos (and ones not yet reparented into them)
    // keep a compact size hint so the 1400 px layout does not squeeze the
    // Property Inspector; long items elide with a tooltip. Honour a
    // tighter maximumWidth the widget already set (e.g. Brush category).
    bool anchored = false;
    for (const QWidget *w = combo; w; w = w->parentWidget()) {
        if (w->inherits("PropertyInspector")
            || w->objectName() == QLatin1String("PropertyInspector")
            || qobject_cast<const QDialog *>(w)
            || qobject_cast<const QMainWindow *>(w)) {
            anchored = true;
            break;
        }
    }
    if (cap >= kDefaultCap && anchored) {
        const QFontMetrics fm(combo->font());
        const int cw = qMax(1, fm.horizontalAdvance(QLatin1Char('x')));
        const int pad = chromeWidth(combo);
        const int chars = qBound(4, (qMin(cap, wantText + pad) - pad + cw - 1) / cw, 36);
        if (combo->minimumContentsLength() < chars)
            combo->setMinimumContentsLength(chars);
    } else {
        combo->setMinimumContentsLength(qMax(4, origMcl));
    }
    combo->setProperty(kFitted, true);
    applyItemTooltips(combo, true);
}

void applyTree(QWidget *root)
{
    if (!root) return;
    const auto combos = root->findChildren<QComboBox *>(Qt::FindChildrenRecursively);
    for (QComboBox *c : combos) apply(c);
    if (auto *c = qobject_cast<QComboBox *>(root)) apply(c);
}

void install(QCoreApplication *app)
{
    if (!app) app = QCoreApplication::instance();
    if (!app) return;
    if (g_filter) return;
    g_filter = new ComboFitFilter(app);
    app->installEventFilter(g_filter);
}

} // namespace combofit
