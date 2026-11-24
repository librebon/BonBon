#include "FileDialogs.h"
#include "AppState.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTimer>
#include <QWidget>

namespace filedialogs {

QFileDialog::Options options(QFileDialog::Options extra) {
    QFileDialog::Options o = extra;
    if (!appState().prefs.useSystemFileDialogs)
        o |= QFileDialog::DontUseNativeDialog;
    return o;
}

static void applyFilterItemTooltips(QComboBox *combo) {
    if (!combo) return;
    for (int i = 0; i < combo->count(); ++i) {
        const QString text = combo->itemText(i);
        if (combo->itemData(i, Qt::ToolTipRole).toString() != text)
            combo->setItemData(i, text, Qt::ToolTipRole);
    }
}

// Popup may be as wide as the dialog, but never past about 60% of the screen.
static int filterPopupCap(const QComboBox *combo) {
    if (!combo) return 0;
    int dlgW = 0;
    if (QWidget *w = combo->window())
        dlgW = w->width();
    int sixty = 0;
    if (QScreen *sc = combo->screen())
        sixty = qRound(double(sc->availableGeometry().width()) * 0.60);
    if (dlgW > 0 && sixty > 0) return qMin(dlgW, sixty);
    if (dlgW > 0) return dlgW;
    return sixty;
}

// Size the file-type popup to its items, capped to filterPopupCap(). Long rows
// stay ElideMiddle so the start and end of the extension list remain visible.
static void capFilterComboPopup(QComboBox *combo) {
    if (!combo) return;
    applyFilterItemTooltips(combo);
    QAbstractItemView *view = combo->view();
    if (!view) return;
    view->setTextElideMode(Qt::ElideMiddle);

    const int cap = filterPopupCap(combo);
    if (cap <= 0) return;

    int widest = 0;
    const QFontMetrics fm(view->font());
    for (int i = 0; i < combo->count(); ++i)
        widest = qMax(widest, fm.horizontalAdvance(combo->itemText(i)));

    QStyleOptionComboBox boxOpt;
    boxOpt.initFrom(combo);
    boxOpt.editable = combo->isEditable();
    const int frame = combo->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &boxOpt, view) * 2;

    QScrollBar *vsb = view->verticalScrollBar();
    if (vsb) vsb->ensurePolished();
    view->ensurePolished();
    int scroll = 20;
    if (vsb) {
        scroll = qMax(vsb->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, vsb),
                      qMax(vsb->sizeHint().width(), vsb->isVisible() ? vsb->width() : 0));
    }
    const QMargins m = view->contentsMargins();
    const int fromItems = widest + scroll + frame + m.left() + m.right() + 8;
    // At least the closed combo, at most the cap; narrower than the dialog when
    // the filter list is short (Save, Batch "Add Files", etc.).
    const int lo = qMin(combo->width(), cap);
    const int viewWant = qBound(lo, fromItems, cap);

    view->setMinimumWidth(0);
    view->setMaximumWidth(cap);

    QWidget *container = view->window();
    const bool popup = container && container != combo->window();
    if (QWidget *vp = view->parentWidget()) {
        if (!popup || vp != container) {
            vp->setMinimumWidth(0);
            vp->setMaximumWidth(cap);
            vp->resize(qMin(qMax(vp->width(), viewWant), cap), vp->height());
        }
    }

    if (popup) {
        container->ensurePolished();
        int cFrame = container->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, container) * 2;
        if (auto *fr = qobject_cast<QFrame *>(container))
            cFrame = qMax(cFrame, fr->frameWidth() * 2);
        const QMargins cm = container->contentsMargins();
        int containerWant = viewWant + cFrame + cm.left() + cm.right();
        if (containerWant <= viewWant)
            containerWant = viewWant + qMax(2, cFrame);
        if (containerWant > cap)
            containerWant = cap;

        container->setMinimumWidth(0);
        container->setMaximumWidth(cap);
        container->resize(containerWant, container->height());

        // Keep a dialog-width popup from hanging past the dialog (the combo sits
        // inset from the left edge). Always stay on the screen as well.
        QRect limit;
        if (QScreen *sc = combo->screen())
            limit = sc->availableGeometry();
        if (QWidget *dlg = combo->window()) {
            const QRect dg = dlg->frameGeometry();
            if (limit.isNull()) {
                limit = dg;
            } else if (containerWant <= dlg->width() + 2) {
                const int left = qMax(limit.left(), dg.left());
                const int right = qMin(limit.right(), dg.right());
                if (right >= left)
                    limit = QRect(QPoint(left, limit.top()), QPoint(right, limit.bottom()));
            }
        }
        if (limit.width() > 0 && limit.height() > 0) {
            QRect g = container->frameGeometry();
            int x = g.x();
            int y = g.y();
            if (g.right() > limit.right())
                x = qMax(limit.left(), limit.right() - g.width() + 1);
            if (x < limit.left())
                x = limit.left();
            if (g.bottom() > limit.bottom())
                y = qMax(limit.top(), limit.bottom() - g.height() + 1);
            if (y < limit.top())
                y = limit.top();
            if (x != container->x() || y != container->y())
                container->move(x, y);
        }
    }
}

class FilterComboPopupFilter final : public QObject {
public:
    explicit FilterComboPopupFilter(QComboBox *combo) : QObject(combo), m_combo(combo) {
        if (combo && combo->view())
            combo->view()->installEventFilter(this);
    }
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Show || !m_combo)
            return false;
        QAbstractItemView *view = m_combo->view();
        if (!view || (watched != view && watched != view->window()))
            return false;
        auto cap = [c = m_combo] { if (c) capFilterComboPopup(c); };
        cap();
        QTimer::singleShot(0, m_combo, cap);
        return false;
    }
private:
    QPointer<QComboBox> m_combo;
};

static void watchFilterComboTooltips(QComboBox *combo) {
    applyFilterItemTooltips(combo);
    if (!combo || combo->property("_lwFilterTips").toBool())
        return;
    combo->setProperty("_lwFilterTips", true);
    auto apply = [combo] { applyFilterItemTooltips(combo); };
    if (QAbstractItemModel *model = combo->model()) {
        QObject::connect(model, &QAbstractItemModel::modelReset, combo, apply);
        QObject::connect(model, &QAbstractItemModel::rowsInserted, combo, apply);
        QObject::connect(model, &QAbstractItemModel::dataChanged, combo, apply);
    }
}

void configureFilterCombo(QFileDialog *dlg) {
    if (!dlg) return;
    QComboBox *combo = dlg->findChild<QComboBox *>(QStringLiteral("fileTypeCombo"));
    if (!combo) return;
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(kFilterComboMinContentsLength);
    combo->setSizePolicy(QSizePolicy::Expanding, combo->sizePolicy().verticalPolicy());
    combo->setMinimumWidth(0);
    if (QAbstractItemView *view = combo->view()) {
        view->setTextElideMode(Qt::ElideMiddle);
        view->setMinimumWidth(0);
    }
    if (!combo->property("_lwFilterPopup").toBool()) {
        combo->setProperty("_lwFilterPopup", true);
        new FilterComboPopupFilter(combo);
        watchFilterComboTooltips(combo);
    }
    applyFilterItemTooltips(combo);
    combo->updateGeometry();
    dlg->updateGeometry();
}

static void applyOptions(QFileDialog &dlg, QFileDialog::Options extra) {
    const bool native = appState().prefs.useSystemFileDialogs;
    dlg.setOption(QFileDialog::DontUseNativeDialog, !native);
    if (extra)
        dlg.setOptions(dlg.options() | extra);
}

// Filters go in only after DontUseNativeDialog + configureFilterCombo. With a
// system platform theme that has a native file-dialog helper (gtk3, KDE,
// portal), QFileDialog builds its widgets lazily inside setOption() and then
// resize()s itself to sizeHint() with the stock AdjustToContentsOnFirstShow
// combo, so a long filter list set in the constructor fixes the width first.
static void setFilters(QFileDialog &dlg, const QString &filter, const QString *selectedFilter) {
    if (dlg.testOption(QFileDialog::DontUseNativeDialog))
        configureFilterCombo(&dlg);
    if (!filter.isEmpty())
        dlg.setNameFilter(filter);
    if (selectedFilter && !selectedFilter->isEmpty())
        dlg.selectNameFilter(*selectedFilter);
    if (dlg.testOption(QFileDialog::DontUseNativeDialog))
        configureFilterCombo(&dlg);
}

static QString execSingle(QFileDialog &dlg, QString *selectedFilter) {
    if (dlg.exec() != QDialog::Accepted)
        return {};
    if (selectedFilter)
        *selectedFilter = dlg.selectedNameFilter();
    const QStringList files = dlg.selectedFiles();
    return files.isEmpty() ? QString() : files.first();
}

QString getOpenFileName(QWidget *parent, const QString &caption, const QString &dir,
                        const QString &filter, QString *selectedFilter,
                        QFileDialog::Options extra) {
    QFileDialog dlg(parent, caption, dir);
    applyOptions(dlg, extra);
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setFileMode(QFileDialog::ExistingFile);
    setFilters(dlg, filter, selectedFilter);
    return execSingle(dlg, selectedFilter);
}

QStringList getOpenFileNames(QWidget *parent, const QString &caption, const QString &dir,
                             const QString &filter, QString *selectedFilter,
                             QFileDialog::Options extra) {
    QFileDialog dlg(parent, caption, dir);
    applyOptions(dlg, extra);
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    dlg.setFileMode(QFileDialog::ExistingFiles);
    setFilters(dlg, filter, selectedFilter);
    if (dlg.exec() != QDialog::Accepted)
        return {};
    if (selectedFilter)
        *selectedFilter = dlg.selectedNameFilter();
    return dlg.selectedFiles();
}

QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir,
                        const QString &filter, QString *selectedFilter,
                        QFileDialog::Options extra) {
    QFileDialog dlg(parent, caption, dir);
    applyOptions(dlg, extra);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    setFilters(dlg, filter, selectedFilter);
    return execSingle(dlg, selectedFilter);
}

QString getExistingDirectory(QWidget *parent, const QString &caption, const QString &dir,
                             QFileDialog::Options extra) {
    QFileDialog dlg(parent, caption, dir);
    applyOptions(dlg, extra);
    dlg.setFileMode(QFileDialog::Directory);
    if (dlg.testOption(QFileDialog::DontUseNativeDialog))
        configureFilterCombo(&dlg);
    if (dlg.exec() != QDialog::Accepted)
        return {};
    return dlg.selectedFiles().value(0);
}

}
