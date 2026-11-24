#pragma once
#include <QSet>
#include <QWidget>
class QComboBox;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;
class QKeySequenceEdit;
class QPushButton;
class QLabel;
class MainWindow;

// Preferences > Keyboard: Fireworks-style shortcut set editor (0.3.6).
class KeyboardPrefsPage : public QWidget {
    Q_OBJECT
public:
    explicit KeyboardPrefsPage(MainWindow *mw, QWidget *parent = nullptr);
    void refillSets();
    void refillTree();
private:
    void updateSetButtons();
    void refillList();
    void applyDefaultTreeExpansion();
    void snapshotExpandedPaths();
    void restoreExpandedPaths();
    static QString treeItemPath(const QTreeWidgetItem *it);
    void duplicateSet();
    void renameSet();
    void deleteSet();
    void addShortcut();
    void removeShortcut();
    void makePrimary();
    void moveShortcutUp();
    void moveShortcutDown();
    void reorderShortcut(int from, int to);
    MainWindow *m_mw = nullptr;
    QComboBox *m_sets = nullptr;
    QTreeWidget *m_tree = nullptr;
    QListWidget *m_list = nullptr;
    QKeySequenceEdit *m_capture = nullptr;
    QPushButton *m_dup = nullptr, *m_ren = nullptr, *m_del = nullptr, *m_add = nullptr, *m_rem = nullptr;
    QPushButton *m_primary = nullptr, *m_up = nullptr, *m_down = nullptr;
    QLabel *m_warn = nullptr;
    QString m_actionId;
    bool m_expandTouched = false;          // user expanded/collapsed during this dialog session
    QSet<QString> m_expandedPaths;         // paths to restore across refillTree when touched
};
