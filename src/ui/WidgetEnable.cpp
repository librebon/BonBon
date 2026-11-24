#include "WidgetEnable.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

namespace uiutil {
namespace {

bool isHeadingLabel(const QLabel *lab)
{
    if (!lab) return false;
    if (lab->buddy()) return false;
    // Checkbox/radio text lives on the button itself, not a separate heading.
    if (lab->wordWrap() && lab->text().size() > 80) return false;
    return true;
}

bool isInteractive(const QWidget *w)
{
    if (!w) return false;
    if (qobject_cast<const QAbstractButton *>(w)
        || qobject_cast<const QAbstractSpinBox *>(w)
        || qobject_cast<const QComboBox *>(w)
        || qobject_cast<const QLineEdit *>(w)
        || qobject_cast<const QAbstractSlider *>(w)
        || qobject_cast<const QPlainTextEdit *>(w)
        || qobject_cast<const QTextEdit *>(w))
        return true;
    return false;
}

bool hasEnabledInteractive(const QWidget *root)
{
    if (!root) return false;
    if (isInteractive(root) && root->isEnabled()) return true;
    const auto kids = root->findChildren<QWidget *>(Qt::FindChildrenRecursively);
    for (const QWidget *c : kids) {
        if (!isInteractive(c)) continue;
        if (c->isEnabled()) return true;
    }
    return false;
}

QLabel *formLabelFor(QWidget *field)
{
    if (!field) return nullptr;
    if (QWidget *parent = field->parentWidget()) {
        if (auto *form = qobject_cast<QFormLayout *>(parent->layout())) {
            if (QWidget *lab = form->labelForField(field))
                return qobject_cast<QLabel *>(lab);
        }
    }
    return nullptr;
}

QLabel *layoutNeighbourLabel(QWidget *w)
{
    if (!w) return nullptr;
    QWidget *parent = w->parentWidget();
    if (!parent) return nullptr;
    QLayout *lay = parent->layout();
    if (!lay) return nullptr;
    if (auto *grid = qobject_cast<QGridLayout *>(lay)) {
        const int idx = grid->indexOf(w);
        if (idx < 0) return nullptr;
        int row = 0, col = 0, rs = 0, cs = 0;
        grid->getItemPosition(idx, &row, &col, &rs, &cs);
        if (col <= 0) return nullptr;
        if (QLayoutItem *item = grid->itemAtPosition(row, col - 1))
            return qobject_cast<QLabel *>(item->widget());
        return nullptr;
    }
    if (auto *box = qobject_cast<QBoxLayout *>(lay)) {
        const int idx = box->indexOf(w);
        if (idx <= 0) return nullptr;
        if (QLayoutItem *item = box->itemAt(idx - 1))
            return qobject_cast<QLabel *>(item->widget());
    }
    return nullptr;
}

QList<QLabel *> directLabelsFor(QWidget *w)
{
    QList<QLabel *> out;
    if (!w) return out;
    auto add = [&](QLabel *lab) {
        if (lab && !out.contains(lab)) out.append(lab);
    };
    add(formLabelFor(w));
    add(layoutNeighbourLabel(w));
    QWidget *scope = w->parentWidget() ? w->parentWidget() : w->window();
    if (scope) {
        for (QLabel *lab : scope->findChildren<QLabel *>(Qt::FindChildrenRecursively)) {
            if (lab->buddy() == w) add(lab);
        }
    }
    return out;
}

class EnableLabelFilter final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() != QEvent::EnabledChange) return false;
        auto *w = qobject_cast<QWidget *>(o);
        if (!w || w->property("_lwSyncingLabel").toBool()) return false;
        if (qobject_cast<QLabel *>(w)) return false;
        syncLabelsFor(w);
        if (QWidget *root = w->window())
            syncSectionLabels(root);
        return false;
    }
};

QPointer<EnableLabelFilter> g_filter;

} // namespace

void syncLabelsFor(QWidget *w)
{
    if (!w) return;
    const bool on = w->isEnabled();
    for (QLabel *lab : directLabelsFor(w)) {
        if (!lab || lab->isEnabled() == on) continue;
        lab->setProperty("_lwSyncingLabel", true);
        lab->setEnabled(on);
        lab->setProperty("_lwSyncingLabel", false);
    }
}

void syncSectionLabels(QWidget *root)
{
    if (!root) return;
    QList<QFormLayout *> forms;
    if (auto *f = qobject_cast<QFormLayout *>(root->layout()))
        forms.append(f);
    forms += root->findChildren<QFormLayout *>(Qt::FindChildrenRecursively);
    for (QFormLayout *form : forms) {
        if (!form) continue;
        for (int i = 0; i < form->rowCount(); ++i) {
            QLayoutItem *fi = form->itemAt(i, QFormLayout::FieldRole);
            QLayoutItem *li = form->itemAt(i, QFormLayout::LabelRole);
            if (!fi || !li) continue;
            QWidget *field = fi->widget();
            QWidget *labw = li->widget();
            if (!field || !labw) continue;
            // A container field (Dash row, stamp row, …) is a section: its
            // heading follows whether any interactive child is on.
            const bool container = !isInteractive(field)
                                   && !field->findChildren<QWidget *>(Qt::FindDirectChildrenOnly).isEmpty();
            if (!container && isInteractive(field))
                continue; // direct labels already handled in syncLabelsFor
            const bool on = hasEnabledInteractive(field);
            if (labw->isEnabled() != on) {
                labw->setProperty("_lwSyncingLabel", true);
                labw->setEnabled(on);
                labw->setProperty("_lwSyncingLabel", false);
            }
        }
    }
    QList<QGroupBox *> boxes;
    if (auto *b = qobject_cast<QGroupBox *>(root))
        boxes.append(b);
    boxes += root->findChildren<QGroupBox *>(Qt::FindChildrenRecursively);
    for (QGroupBox *box : boxes) {
        if (!box) continue;
        bool any = isInteractive(box);
        if (!any) {
            for (const QWidget *c : box->findChildren<QWidget *>(Qt::FindChildrenRecursively)) {
                if (isInteractive(c)) { any = true; break; }
            }
        }
        if (!any) continue;
        const bool on = hasEnabledInteractive(box);
        // Group boxes inherit enabledness to their children. Turning the box
        // off only when every child is already off keeps child flags intact.
        if (box->isEnabled() != on)
            box->setEnabled(on);
    }
    Q_UNUSED(isHeadingLabel);
}

void setEnabledWithLabel(QWidget *w, bool on)
{
    if (!w) return;
    installEnableLabelFilter();
    w->setEnabled(on);
    syncLabelsFor(w);
    if (QWidget *root = w->window() ? w->window() : w)
        syncSectionLabels(root);
}

void installEnableLabelFilter(QCoreApplication *app)
{
    if (!app) app = QCoreApplication::instance();
    if (!app) return;
    if (g_filter) return;
    g_filter = new EnableLabelFilter(app);
    app->installEventFilter(g_filter);
}

} // namespace uiutil
