#include "Brand.h"
#include "KeyboardPrefs.h"
#include "AppState.h"
#include "Shortcuts.h"
#include "MainWindow.h"
#include <QCoreApplication>
#include <QAbstractItemView>
#include <functional>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QColor>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTreeWidget>
#include <QHeaderView>
#include <QEvent>
#include <QVBoxLayout>

KeyboardPrefsPage::KeyboardPrefsPage(MainWindow *mw, QWidget *parent) : QWidget(parent), m_mw(mw) {
    connect(&appState(), &AppState::prefsChanged, this, [this] { refillTree(); refillList(); });
    setObjectName(QStringLiteral("prefsKeyboardPage"));
    auto *v = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Current set:")));
    m_sets = new QComboBox;
    m_sets->setObjectName(QStringLiteral("prefShortcutSet"));
    top->addWidget(m_sets, 1);
    m_dup = new QPushButton(QStringLiteral("Duplicate..."));
    m_dup->setObjectName(QStringLiteral("prefShortcutDup"));
    m_ren = new QPushButton(QStringLiteral("Rename..."));
    m_ren->setObjectName(QStringLiteral("prefShortcutRename"));
    m_del = new QPushButton(QStringLiteral("Delete"));
    m_del->setObjectName(QStringLiteral("prefShortcutDelete"));
    top->addWidget(m_dup); top->addWidget(m_ren); top->addWidget(m_del);
    v->addLayout(top);

    auto *split = new QSplitter;
    split->setObjectName(QStringLiteral("prefShortcutSplitter"));
    split->setHandleWidth(8);
    split->setChildrenCollapsible(false);
    m_tree = new QTreeWidget;
    m_tree->setObjectName(QStringLiteral("prefShortcutTree"));
    m_tree->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Shortcut")});
    m_tree->setColumnWidth(0, 280);
    m_tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Scrollbar thumb-beside-header is handled globally by Theme.cpp QScrollBar rules (0.3.15).
    split->addWidget(m_tree);
    m_tree->header()->setStretchLastSection(true);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(new QLabel(QStringLiteral("Shortcuts for selected command:")));
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("prefShortcutList"));
    rv->addWidget(m_list, 1);
    auto *row = new QHBoxLayout;
    m_capture = new QKeySequenceEdit;
    m_capture->setObjectName(QStringLiteral("prefShortcutCapture"));
    m_add = new QPushButton(QStringLiteral("+"));
    m_add->setObjectName(QStringLiteral("prefShortcutAdd"));
    m_add->setFixedWidth(28);
    m_rem = new QPushButton(QStringLiteral("\u2212"));
    m_rem->setObjectName(QStringLiteral("prefShortcutRemove"));
    m_rem->setFixedWidth(28);
    row->addWidget(m_capture, 1); row->addWidget(m_add); row->addWidget(m_rem);
    rv->addLayout(row);
    auto *ord = new QHBoxLayout;
    m_primary = new QPushButton(QStringLiteral("Make primary"));
    m_primary->setObjectName(QStringLiteral("prefShortcutPrimary"));
    m_primary->setToolTip(QStringLiteral("Move the selected shortcut to the top — tooltips, flyouts and menus show this one first"));
    m_primary->setStyleSheet(QStringLiteral("QPushButton:enabled { font-weight: 600; }"));
    m_up = new QPushButton(QStringLiteral("Move up"));
    m_up->setObjectName(QStringLiteral("prefShortcutUp"));
    m_down = new QPushButton(QStringLiteral("Move down"));
    m_down->setObjectName(QStringLiteral("prefShortcutDown"));
    ord->addWidget(m_primary); ord->addWidget(m_up); ord->addWidget(m_down); ord->addStretch(1);
    rv->addLayout(ord);
    m_warn = new QLabel;
    m_warn->setObjectName(QStringLiteral("prefShortcutWarn"));
    m_warn->setWordWrap(true);
    m_warn->setStyleSheet(QStringLiteral("color:#a40;"));
    rv->addWidget(m_warn);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    refillSets();
    refillTree();
    connect(m_sets, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QString id = m_sets->itemData(i).toString();
        shortcuts::Manager::get().setCurrent(id);
        updateSetButtons();
        refillTree();
        if (m_mw) shortcuts::Manager::get().apply(m_mw);
    });
    connect(m_dup, &QPushButton::clicked, this, &KeyboardPrefsPage::duplicateSet);
    connect(m_ren, &QPushButton::clicked, this, &KeyboardPrefsPage::renameSet);
    connect(m_del, &QPushButton::clicked, this, &KeyboardPrefsPage::deleteSet);
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) {
        if (m_tree->signalsBlocked()) return;
        m_expandTouched = true;
        m_expandedPaths.insert(treeItemPath(it));
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *it) {
        if (m_tree->signalsBlocked()) return;
        m_expandTouched = true;
        m_expandedPaths.remove(treeItemPath(it));
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it, QTreeWidgetItem *) {
        m_actionId.clear();
        if (it && !it->data(0, Qt::UserRole).toString().isEmpty()) {
            m_actionId = it->data(0, Qt::UserRole).toString();
            refillList();
        } else { m_list->clear(); updateSetButtons(); }
    });
    connect(m_add, &QPushButton::clicked, this, &KeyboardPrefsPage::addShortcut);
    connect(m_rem, &QPushButton::clicked, this, &KeyboardPrefsPage::removeShortcut);
    connect(m_primary, &QPushButton::clicked, this, &KeyboardPrefsPage::makePrimary);
    connect(m_up, &QPushButton::clicked, this, &KeyboardPrefsPage::moveShortcutUp);
    connect(m_down, &QPushButton::clicked, this, &KeyboardPrefsPage::moveShortcutDown);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { updateSetButtons(); });
    updateSetButtons();
}

void KeyboardPrefsPage::refillSets() {
    auto &m = shortcuts::Manager::get();
    const QString cur = m.currentId();
    m_sets->blockSignals(true);
    m_sets->clear();
    int select = 0;
    for (const shortcuts::Set &s : m.sets()) {
        const QString label = s.name + (s.readOnly ? QStringLiteral(" (default)") : QString());
        m_sets->addItem(label, s.id);
        if (s.id == cur) select = m_sets->count() - 1;
    }
    m_sets->setCurrentIndex(select);
    m_sets->blockSignals(false);
    updateSetButtons();
}

void KeyboardPrefsPage::updateSetButtons() {
    const shortcuts::Set *s = shortcuts::Manager::get().set(shortcuts::Manager::get().currentId());
    const bool ro = !s || s->readOnly;
    // Rename / Delete only apply to custom sets; shortcut editing is always available (auto-forks the default)
    m_ren->setEnabled(!ro);
    m_del->setEnabled(!ro);
    m_add->setEnabled(true);
    m_rem->setEnabled(true);
    const int row = m_list ? m_list->currentRow() : -1;
    const int n = m_list ? m_list->count() : 0;
    if (m_primary) m_primary->setEnabled(row > 0);
    if (m_up) m_up->setEnabled(row > 0);
    if (m_down) m_down->setEnabled(row >= 0 && row < n - 1);
    m_capture->setEnabled(true);
    m_capture->setFocusPolicy(Qt::StrongFocus);
    // QKeySequenceEdit greys its inner line edit when disabled; keep that child on too
    if (auto *le = m_capture->findChild<QLineEdit *>()) {
        le->setEnabled(true);
        le->setReadOnly(false);
    }
    if (m_warn) {
        m_warn->setText(ro
            ? QStringLiteral("%1 is the built-in default. Changing a shortcut creates “%1 (custom)” (or “custom 2”, …), switches to it, and applies the edit. You can rename that set.").arg(brand::name())
            : QString());
    }
}

void KeyboardPrefsPage::refillTree() {
    const QString keep = m_actionId;
    if (m_expandTouched) snapshotExpandedPaths();
    m_tree->blockSignals(true);
    m_tree->clear();
    auto &m = shortcuts::Manager::get();
    auto *menuRoot = new QTreeWidgetItem(m_tree, {QStringLiteral("Menu Commands")});
    auto *toolsRoot = new QTreeWidgetItem(m_tree, {QStringLiteral("Tools")});
    auto *miscRoot = new QTreeWidgetItem(m_tree, {QStringLiteral("Miscellaneous")});
    QHash<QString, QTreeWidgetItem *> menuNodes{{QString(), menuRoot}};
    QTreeWidgetItem *restore = nullptr;
    QSet<QString> toolLabels;
    for (const shortcuts::ActionInfo &a : m.catalog()) {
        if (isWebFeatureAction(a.id) && !webFeaturesEnabled()) continue;
        if (a.category == QLatin1String("tools")) toolLabels.insert(a.label);
    }
    for (const shortcuts::ActionInfo &a : m.catalog()) {
        if (isWebFeatureAction(a.id) && !webFeaturesEnabled()) continue;
        QTreeWidgetItem *parent = miscRoot;
        if (a.category == QLatin1String("tools")) parent = toolsRoot;
        else if (a.category == QLatin1String("menu")) {
            const QString path = a.menuPath.section(QLatin1Char('/'), 0, -2);
            if (!menuNodes.contains(path)) {
                QTreeWidgetItem *p = menuRoot;
                QString acc;
                for (const QString &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                    acc = acc.isEmpty() ? part : acc + QLatin1Char('/') + part;
                    if (!menuNodes.contains(acc)) {
                        auto *n = new QTreeWidgetItem(p, {part});
                        menuNodes.insert(acc, n);
                    }
                    p = menuNodes.value(acc);
                }
            }
            parent = menuNodes.value(path, menuRoot);
        }
        const QList<QKeySequence> seqs = m.sequences(a.id);
        QStringList parts;
        for (const QKeySequence &k : seqs)
            if (!k.isEmpty()) parts << k.toString(QKeySequence::NativeText);
        // Modify > Transform > Scale etc. share short names with Tools; show the menu path.
        QString label = a.label;
        if (a.category == QLatin1String("menu") && toolLabels.contains(a.label) && !a.menuPath.isEmpty())
            label = QString(a.menuPath).replace(QLatin1Char('/'), QStringLiteral(" › "));
        auto *it = new QTreeWidgetItem(parent, {label, parts.join(QStringLiteral(", "))});
        it->setData(0, Qt::UserRole, a.id);
        if (a.category == QLatin1String("menu") && !a.menuPath.isEmpty())
            it->setToolTip(0, a.menuPath);
        if (!keep.isEmpty() && a.id == keep) restore = it;
    }
    auto *modRoot = new QTreeWidgetItem(m_tree, {QStringLiteral("Modifiers (not editable)"), QStringLiteral("—")});
    modRoot->setFlags(modRoot->flags() & ~Qt::ItemIsSelectable);
    modRoot->setToolTip(0, QStringLiteral("Held during a drag or click. These stay as modifiers and cannot be remapped."));
    for (const shortcuts::ModifierNote &n : m.modifierNotes()) {
        auto *it = new QTreeWidgetItem(modRoot, {QStringLiteral("%1 — %2").arg(n.when, n.effect), n.keys});
        it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
        it->setForeground(0, QColor(0x66, 0x66, 0x66));
        it->setForeground(1, QColor(0x66, 0x66, 0x66));
    }
    // Default: only Tools expanded (nested Menu Commands submenus stay closed too).
    // If the user expanded/collapsed anything this session, restore that instead.
    if (m_expandTouched) restoreExpandedPaths();
    else applyDefaultTreeExpansion();
    m_tree->blockSignals(false);
    if (restore) {
        // Ensure ancestors of the kept selection are expanded so it stays visible.
        for (QTreeWidgetItem *p = restore->parent(); p; p = p->parent())
            m_tree->expandItem(p);
        m_tree->setCurrentItem(restore);
        m_actionId = keep;
        m_tree->scrollToItem(restore, QAbstractItemView::EnsureVisible);
    } else if (!m_expandTouched) {
        // Fresh open / set switch: selection and scroll at the top of Tools.
        if (toolsRoot->childCount() > 0) {
            QTreeWidgetItem *first = toolsRoot->child(0);
            m_tree->setCurrentItem(first);
            m_actionId = first->data(0, Qt::UserRole).toString();
            refillList();
        } else {
            m_tree->setCurrentItem(toolsRoot);
        }
        m_tree->scrollToItem(toolsRoot, QAbstractItemView::PositionAtTop);
    }
}

QString KeyboardPrefsPage::treeItemPath(const QTreeWidgetItem *it) {
    QStringList parts;
    for (const QTreeWidgetItem *p = it; p; p = p->parent())
        parts.prepend(p->text(0));
    return parts.join(QLatin1Char('/'));
}

void KeyboardPrefsPage::snapshotExpandedPaths() {
    m_expandedPaths.clear();
    if (!m_tree) return;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) {
        if (!it) return;
        if (it->isExpanded()) m_expandedPaths.insert(treeItemPath(it));
        for (int i = 0; i < it->childCount(); ++i) walk(it->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) walk(m_tree->topLevelItem(i));
}

void KeyboardPrefsPage::restoreExpandedPaths() {
    if (!m_tree) return;
    // Collapse everything first, then re-open recorded paths (and Tools always).
    std::function<void(QTreeWidgetItem *)> collapseAll = [&](QTreeWidgetItem *it) {
        if (!it) return;
        for (int i = 0; i < it->childCount(); ++i) collapseAll(it->child(i));
        m_tree->collapseItem(it);
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) collapseAll(m_tree->topLevelItem(i));
    std::function<void(QTreeWidgetItem *)> expandMatch = [&](QTreeWidgetItem *it) {
        if (!it) return;
        const QString path = treeItemPath(it);
        if (m_expandedPaths.contains(path) || it->text(0) == QLatin1String("Tools"))
            m_tree->expandItem(it);
        for (int i = 0; i < it->childCount(); ++i) expandMatch(it->child(i));
    };
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) expandMatch(m_tree->topLevelItem(i));
}

void KeyboardPrefsPage::applyDefaultTreeExpansion() {
    if (!m_tree) return;
    std::function<void(QTreeWidgetItem *)> collapseAll = [&](QTreeWidgetItem *it) {
        if (!it) return;
        for (int i = 0; i < it->childCount(); ++i) collapseAll(it->child(i));
        m_tree->collapseItem(it);
    };
    QTreeWidgetItem *toolsRoot = nullptr;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_tree->topLevelItem(i);
        collapseAll(it);
        if (it->text(0) == QLatin1String("Tools")) toolsRoot = it;
    }
    if (toolsRoot) m_tree->expandItem(toolsRoot);
}

void KeyboardPrefsPage::refillList() {
    m_list->clear();
    if (m_actionId.isEmpty()) { updateSetButtons(); return; }
    for (const QKeySequence &k : shortcuts::Manager::get().sequences(m_actionId))
        m_list->addItem(k.toString(QKeySequence::NativeText));
    updateSetButtons();
}

void KeyboardPrefsPage::duplicateSet() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Duplicate Shortcut Set"), QStringLiteral("Name:"),
                                               QLineEdit::Normal, shortcuts::Manager::get().set(shortcuts::Manager::get().currentId())->name + QStringLiteral(" copy"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    shortcuts::Manager::get().duplicate(shortcuts::Manager::get().currentId(), name.trimmed());
    refillSets();
    refillTree();
    if (m_mw) shortcuts::Manager::get().apply(m_mw);
}

void KeyboardPrefsPage::renameSet() {
    auto *s = shortcuts::Manager::get().set(shortcuts::Manager::get().currentId());
    if (!s || s->readOnly) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Shortcut Set"), QStringLiteral("Name:"), QLineEdit::Normal, s->name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (!shortcuts::Manager::get().rename(s->id, name.trimmed())) {
        QMessageBox::warning(this, QStringLiteral("Rename"), QStringLiteral("That name is already used."));
        return;
    }
    refillSets();
}

void KeyboardPrefsPage::deleteSet() {
    auto *s = shortcuts::Manager::get().set(shortcuts::Manager::get().currentId());
    if (!s || s->readOnly) return;
    if (QMessageBox::question(this, QStringLiteral("Delete Shortcut Set"),
                              QStringLiteral("Delete “%1”?").arg(s->name)) != QMessageBox::Yes) return;
    shortcuts::Manager::get().remove(s->id);
    refillSets();
    refillTree();
    if (m_mw) shortcuts::Manager::get().apply(m_mw);
}

void KeyboardPrefsPage::addShortcut() {
    if (m_actionId.isEmpty()) return;
    const QKeySequence seq = m_capture->keySequence();
    if (seq.isEmpty()) return;
    auto &m = shortcuts::Manager::get();
    bool forked = false;
    const QString clash = m.conflict(seq, m_actionId);
    if (!clash.isEmpty()) {
        const auto r = QMessageBox::warning(this, QStringLiteral("Shortcut Conflict"),
            QStringLiteral("“%1” is already used by “%2”.\nReassign it to this command?")
                .arg(seq.toString(QKeySequence::NativeText), clash),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) return;
        QList<QKeySequence> other = m.sequences(clash);
        other.removeAll(seq);
        forked = m.setSequences(clash, other) || forked;
    }
    QList<QKeySequence> seqs = m.sequences(m_actionId);
    if (!seqs.contains(seq)) {
        // Tip / flyout show the first shortcut. If the action still has only its built-in
        // default(s), replace them with the new key ("I set Scale to S"). Otherwise prepend
        // so the key just typed becomes primary immediately (Make primary still available).
        const QList<QKeySequence> defs = m.defaultSequences(m_actionId);
        bool onlyDefaults = (seqs.size() == defs.size());
        if (onlyDefaults) {
            for (int i = 0; i < seqs.size(); ++i)
                if (seqs[i] != defs.value(i)) { onlyDefaults = false; break; }
        }
        if (onlyDefaults && !defs.isEmpty()) seqs = {seq};
        else seqs.prepend(seq);
    }
    forked = m.setSequences(m_actionId, seqs) || forked;
    m_capture->clear();
    // Same path as Duplicate: always refresh the Current set combo after a possible auto-fork
    refillSets();
    refillTree();
    refillList();
    if (m_mw) m.apply(m_mw);
    Q_UNUSED(forked);
}

void KeyboardPrefsPage::removeShortcut() {
    if (m_actionId.isEmpty() || !m_list->currentItem()) return;
    auto &m = shortcuts::Manager::get();
    QList<QKeySequence> seqs = m.sequences(m_actionId);
    const int row = m_list->currentRow();
    if (row < 0 || row >= seqs.size()) return;
    seqs.removeAt(row);
    m.setSequences(m_actionId, seqs);
    refillSets();
    refillTree();
    refillList();
    if (m_mw) m.apply(m_mw);
}

void KeyboardPrefsPage::reorderShortcut(int from, int to) {
    if (m_actionId.isEmpty() || from < 0 || to < 0) return;
    auto &m = shortcuts::Manager::get();
    QList<QKeySequence> seqs = m.sequences(m_actionId);
    if (from >= seqs.size() || to >= seqs.size() || from == to) return;
    seqs.move(from, to);
    m.setSequences(m_actionId, seqs);
    refillSets();
    refillTree();
    refillList();
    m_list->setCurrentRow(to);
    if (m_mw) m.apply(m_mw);
}

void KeyboardPrefsPage::makePrimary() {
    const int row = m_list->currentRow();
    if (row <= 0) return;
    reorderShortcut(row, 0);
}

void KeyboardPrefsPage::moveShortcutUp() {
    const int row = m_list->currentRow();
    if (row <= 0) return;
    reorderShortcut(row, row - 1);
}

void KeyboardPrefsPage::moveShortcutDown() {
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_list->count() - 1) return;
    reorderShortcut(row, row + 1);
}
