#include "WideFontCombo.h"
#include "AppState.h"
#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QListView>
#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QFontInfo>
#include <QBrush>
#include <QColor>
#include <QFontMetrics>
#include <QFrame>
#include <QPointer>
#include <QScrollBar>
#include <QScreen>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QSet>
#include <QCompleter>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QSignalBlocker>


void fillFontSourceCombo(QComboBox *combo, bool includeEmpty) {
    if (!combo) return;
    const QString cur = combo->currentText();
    combo->clear();
    if (includeEmpty) combo->addItem(QString());
    combo->addItems(appState().fontSourceFamilies());
    if (!cur.isEmpty()) {
        int i = combo->findText(cur, Qt::MatchExactly);
        if (i < 0) {
            // Case-insensitive search, then keep the excluded family as an entry.
            for (int r = 0; r < combo->count(); ++r) {
                if (combo->itemText(r).compare(cur, Qt::CaseInsensitive) == 0) { i = r; break; }
            }
        }
        if (i < 0) {
            combo->insertItem(includeEmpty ? 1 : 0, cur);
            i = combo->findText(cur, Qt::MatchExactly);
        }
        if (i >= 0) combo->setCurrentIndex(i);
        else if (combo->isEditable()) combo->setEditText(cur);
    }
}

WideFontComboBox::WideFontComboBox(QWidget *parent) : QFontComboBox(parent) {
    setObjectName(QStringLiteral("wideFontCombo"));
    connect(&appState(), &AppState::prefsChanged, this, &WideFontComboBox::refreshFontSources);
    // After QFontComboBox fills its model (which happens at polish / show).
    QTimer::singleShot(0, this, &WideFontComboBox::refreshFontSources);
    // Remember a user pick; ignore programmatic resets (theme / application font).
    connect(this, &QComboBox::activated, this, [this](int) {
        const QString fam = currentFont().family();
        if (!fam.isEmpty()) m_stickyFamily = fam;
    });
}

static bool isGenericFontFamily(const QString &fam);
static bool familyIsInstalled(const QString &fam);
static void markFamilyItem(QStandardItem *it, const QString &fam, bool missing);

void WideFontComboBox::refreshFontSources() {
    if (!model() && !m_list) return;
    const QSignalBlocker blocker(this);
    const QString cur = currentFamily().isEmpty() ? currentFont().family() : currentFamily();
    // QFontComboBox reports generic faces such as "Sans Serif" as the current
    // font before a real family is chosen. Do not sticky those automatically
    // or they leak into a filtered (custom-only) list as an extra kept entry.
    if (m_stickyFamily.isEmpty() && !cur.isEmpty() && !isGenericFontFamily(cur))
        m_stickyFamily = cur;
    const QString keep = !m_stickyFamily.isEmpty() ? m_stickyFamily : cur;

    // Build the list ourselves. QFontComboBox's model is filled at construction and
    // does not reliably pick up addApplicationFont(); it also injects generic
    // faces such as "Sans Serif" that would otherwise leak through a filter.
    QStringList families = appState().fontSourceFamilies();
    if (!keep.isEmpty()) {
        bool found = false;
        for (const QString &f : families) {
            if (f.compare(keep, Qt::CaseInsensitive) == 0) { found = true; break; }
        }
        if (!found)
            families.prepend(keep);
    }

    if (!m_list)
        m_list = new QStandardItemModel(this);
    m_list->clear();
    for (const QString &fam : families) {
        if (fam.isEmpty()) continue;
        auto *it = new QStandardItem;
        markFamilyItem(it, fam, !familyIsInstalled(fam));
        m_list->appendRow(it);
    }

    // QComboBox::setModel() deletes the previous model when the combo owns it.
    if (model() != m_list) {
        if (QAbstractItemModel *old = model()) {
            if (old != m_list && old->parent() == this)
                old->setParent(m_list);
        }
        setModel(m_list);
    }
    if (QCompleter *c = completer()) c->setModel(m_list);
    if (!keep.isEmpty())
        setCurrentFamily(keep);
}

static bool isGenericFontFamily(const QString &fam)
{
    // QFont / QFontComboBox placeholders. A theme or application-font change
    // often reports one of these, which must not evict a real chosen family.
    static const char *const kNames[] = {
        "Sans Serif", "Serif", "Monospace", "TypeWriter", "Decorative",
        "Any", "Cursive", "Fantasy", "System", "MS Sans Serif", "MS Serif"
    };
    for (const char *n : kNames)
        if (fam.compare(QLatin1String(n), Qt::CaseInsensitive) == 0) return true;
    return false;
}

static bool familyIsInstalled(const QString &fam)
{
    if (fam.isEmpty()) return false;
    if (isGenericFontFamily(fam)) return true;
    const QStringList all = QFontDatabase::families();
    for (const QString &f : all) {
        if (f.compare(fam, Qt::CaseInsensitive) == 0)
            return true;
    }
    // QFontInfo reports the face actually used for drawing.
    const QString resolved = QFontInfo(QFont(fam)).family();
    return !resolved.isEmpty() && resolved.compare(fam, Qt::CaseInsensitive) == 0;
}

static void markFamilyItem(QStandardItem *it, const QString &fam, bool missing)
{
    if (!it) return;
    it->setData(fam, Qt::UserRole);
    it->setData(QFont(fam), Qt::FontRole);
    QFont sample(fam);
    if (missing) {
        sample.setItalic(true);
        // Keep the item text as the real family so findText / hasFamily / PI
        // matching still work. Mark missing with italic, grey, and a tooltip.
        it->setText(fam);
        it->setFont(sample);
        it->setForeground(QBrush(QColor(0x88, 0x88, 0x88)));
        it->setToolTip(QStringLiteral("The font “%1” is not installed (missing). The name is kept so the object does not silently switch to another face.").arg(fam));
    } else {
        it->setText(fam);
        it->setFont(sample);
        it->setForeground(QBrush());
        it->setToolTip(QString());
    }
}

QString WideFontComboBox::currentFamily() const
{
    const QVariant stored = currentData(Qt::UserRole);
    if (stored.isValid()) {
        const QString s = stored.toString();
        if (!s.isEmpty()) return s;
    }
    const QString text = currentText();
    // Strip a " (missing)" suffix we may have added for display.
    const QString tag = QStringLiteral(" (missing)");
    if (text.endsWith(tag))
        return text.left(text.size() - tag.size());
    if (!text.isEmpty()) return text;
    return currentFont().family();
}

QFont WideFontComboBox::currentFont() const
{
    QFont f = QFontComboBox::currentFont();
    const QString fam = currentFamily();
    if (!fam.isEmpty())
        f.setFamily(fam);
    return f;
}

void WideFontComboBox::setCurrentFamily(const QString &family)
{
    const QString fam = family.trimmed();
    if (fam.isEmpty()) return;
    m_stickyFamily = fam;
    const bool missing = !familyIsInstalled(fam);
    // Do not block signals here: a caller such as the test or a user-facing
    // setCurrentFont must be able to update the selected object. Callers that
    // are syncing the combo from the object (refreshObject, refreshFontSources,
    // tool-option sync) already block or set m_updating.
    if (!m_list) {
        // Model not built yet (pre-show). Remember the family; refreshFontSources
        // will apply it once the list exists.
        return;
    }
    int row = -1;
    for (int i = 0; i < m_list->rowCount(); ++i) {
        QStandardItem *it = m_list->item(i);
        if (!it) continue;
        const QString stored = it->data(Qt::UserRole).toString();
        const QString label = stored.isEmpty() ? it->text() : stored;
        if (label.compare(fam, Qt::CaseInsensitive) == 0) { row = i; break; }
    }
    if (row < 0) {
        auto *it = new QStandardItem;
        markFamilyItem(it, fam, missing);
        m_list->insertRow(0, it);
        row = 0;
    } else {
        markFamilyItem(m_list->item(row), fam, missing);
    }
    setCurrentIndex(row);
}

void WideFontComboBox::setCurrentFont(const QFont &font)
{
    const QString fam = font.family();
    if (fam.isEmpty()) {
        QFontComboBox::setCurrentFont(font);
        return;
    }
    // Honor the requested family even if it is a generic name such as
    // "Sans Serif" (the default for text objects). The generic-family
    // guard used to live here and ignored PI sync after a document load,
    // leaving the combo on a previously sticky installed face.
    setCurrentFamily(fam);
}

bool WideFontComboBox::event(QEvent *e)
{
    // QFontComboBox reacts to the application font by changing currentFont(),
    // which would rewrite a selected text object and pollute the family list.
    if (e->type() == QEvent::ApplicationFontChange || e->type() == QEvent::FontChange) {
        const QFont keep = currentFont();
        const QSignalBlocker blocker(this);
        const bool r = QFontComboBox::event(e);
        if (!keep.family().isEmpty() && currentFont().family() != keep.family())
            QFontComboBox::setCurrentFont(keep);
        return r;
    }
    return QFontComboBox::event(e);
}

void WideFontComboBox::showPopup() {
    refreshFontSources();
    QFontComboBox::showPopup();
    // Qt styles may resize the container during showPopup; size it afterwards (twice to
    // win any late layout pass inside the same call stack), then once more on a zero timer.
    widenFontPopup(this);
    widenFontPopup(this);
    QPointer<QComboBox> self(this);
    auto reclamp = [self] { if (self) widenFontPopup(self); };
    QTimer::singleShot(0, this, reclamp);
    QTimer::singleShot(1, this, reclamp);
}

int widenFontPopup(QComboBox *combo) {
    if (!combo) return 0;
    QAbstractItemView *view = combo->view();
    if (!view) return 0;

    int widest = 0;
    const int rows = combo->count();
    QStyleOptionViewItem opt;
    opt.initFrom(view);
    opt.features |= QStyleOptionViewItem::HasDisplay;
    const QSize iconSz = view->iconSize().isValid() ? view->iconSize() : QSize(16, 16);

    for (int i = 0; i < rows; ++i) {
        const QModelIndex idx = combo->model()->index(i, combo->modelColumn(), view->rootIndex());
        if (!idx.isValid()) continue;
        QSize hint;
        if (QAbstractItemDelegate *del = view->itemDelegate())
            hint = del->sizeHint(opt, idx);
        if (!hint.isValid() || hint.width() <= 0) {
            const QString text = idx.data(Qt::DisplayRole).toString();
            QFont f = qvariant_cast<QFont>(idx.data(Qt::FontRole));
            if (f.family().isEmpty()) {
                // QFontComboBox paints each row in that family; plain lists use the view font
                if (qobject_cast<QFontComboBox *>(combo) && !text.isEmpty()) f = QFont(text);
                else f = view->font();
            }
            hint = QSize(QFontMetrics(f).horizontalAdvance(text) + 12, QFontMetrics(f).height() + 2);
        }
        int iconW = 0;
        const QVariant dec = idx.data(Qt::DecorationRole);
        if (dec.isValid() && !dec.isNull()) iconW = iconSz.width() + 8;
        // QFontComboBox always draws a sample; budget for it even without DecorationRole
        if (qobject_cast<QFontComboBox *>(combo)) iconW = qMax(iconW, iconSz.width() + 8);
        widest = qMax(widest, hint.width() + iconW);
    }

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
    const int pad = 4;
    const QMargins m = view->contentsMargins();

    const int fromItems = widest + scroll + frame + m.left() + m.right() + pad;
    const int fromCombo = qRound(double(combo->width()) * kFontPopupMinFactor);
    const int viewWant = qMax(qMax(fromItems, fromCombo), combo->width());

    view->setMinimumWidth(viewWant);
    view->setMaximumWidth(QWIDGETSIZE_MAX);

    QWidget *container = view->window();
    const bool popup = container && container != combo->window();
    if (QWidget *vp = view->parentWidget()) {
        if (!popup || vp != container) {
            vp->setMinimumWidth(viewWant);
            vp->resize(qMax(vp->width(), viewWant), vp->height());
        }
    }

    if (popup) {
        container->ensurePolished();
        int cFrame = container->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, container) * 2;
        if (auto *fr = qobject_cast<QFrame *>(container))
            cFrame = qMax(cFrame, fr->frameWidth() * 2);
        const QMargins cm = container->contentsMargins();
        int containerWant = viewWant + cFrame + cm.left() + cm.right();
        // Never assign the same pixel width to the view and the container: the container
        // frame would clip the scrollbar.
        if (containerWant <= viewWant) containerWant = viewWant + qMax(2, cFrame);

        if (QScreen *sc = combo->screen()) {
            const QRect avail = sc->availableGeometry();
            if (containerWant > avail.width())
                containerWant = avail.width();
        }

        container->setMinimumWidth(containerWant);
        container->resize(containerWant, container->height());

        if (QScreen *sc = combo->screen()) {
            const QRect avail = sc->availableGeometry();
            // Cap the layout so a late style pass cannot grow the popup off-screen.
            container->setMaximumWidth(avail.width());
            container->setMaximumHeight(avail.height());
            auto clampTo = [&](QWidget *w) {
                QRect g = w->frameGeometry();
                const int fw = qMax(0, g.width() - w->width());
                const int fh = qMax(0, g.height() - w->height());
                // Remaining room from the current origin (popups may ignore move()).
                int roomW = avail.x() + avail.width() - g.x() - fw;
                int roomH = avail.y() + avail.height() - g.y() - fh;
                if (roomW < 1) { w->move(avail.x(), w->y()); g = w->frameGeometry(); roomW = avail.width() - fw; }
                if (roomH < 1) { w->move(w->x(), avail.y()); g = w->frameGeometry(); roomH = avail.height() - fh; }
                const int nw = qMin(w->width(), qMax(1, roomW));
                const int nh = qMin(w->height(), qMax(1, roomH));
                w->setMaximumWidth(qMax(1, avail.width() - fw));
                w->setMaximumHeight(qMax(1, nh));
                w->resize(nw, nh);
                // If move is honoured, also shift into the available rect when that fits the size.
                g = w->frameGeometry();
                int x = g.x(), y = g.y();
                if (g.x() + g.width() > avail.x() + avail.width())
                    x = qMax(avail.x(), avail.x() + avail.width() - g.width());
                if (g.y() + g.height() > avail.y() + avail.height())
                    y = qMax(avail.y(), avail.y() + avail.height() - g.height());
                if (x < avail.x()) x = avail.x();
                if (y < avail.y()) y = avail.y();
                if (x != w->x() || y != w->y())
                    w->move(x, y);
            };
            clampTo(container);
        }
    }
    return viewWant;
}
