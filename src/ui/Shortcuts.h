#pragma once
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

class QAction;
class QKeyEvent;
class QMainWindow;
class QSettings;

// Fireworks-style keyboard shortcut sets. Every registered action is editable. Editing the
// built-in set auto-forks a "(custom)" copy and switches to it.
// Settings format 2: custom sets store only overrides. Missing action = inherit defaults;
// explicit empty list = deliberate unbind. Format 1 empties that hid later defaults are migrated.
namespace shortcuts {

struct Set {
    QString id;
    QString name;
    bool readOnly = false;   // only the built-in seed; edits fork instead of blocking
    QHash<QString, QList<QKeySequence>> keys; // action id -> shortcuts
};

struct ActionInfo {
    QString id;
    QString label;
    QString category;   // "menu", "tools", "misc"
    QString menuPath;   // Menu Commands tree path, e.g. "File/Export Selected..."
};

struct ModifierNote {
    QString when;    // e.g. "While dragging a fill handle"
    QString keys;    // e.g. "Shift"
    QString effect;  // e.g. "Scale all handles together"
};

class Manager {
public:
    static Manager &get();

    void captureDefaults(QMainWindow *win);          // once after menus/tools are built
    void load(QSettings &st);
    void save(QSettings &st) const;
    void apply(QMainWindow *win) const;              // push current set onto QActions

    QString currentId() const { return m_current; }
    void setCurrent(const QString &id);
    QVector<Set> sets() const { return m_sets; }
    const Set *set(const QString &id) const;
    Set *set(const QString &id);

    QString duplicate(const QString &id, const QString &newName);
    bool rename(const QString &id, const QString &newName);
    bool remove(const QString &id);

    QList<QKeySequence> sequences(const QString &actionId) const;
    // Live captureDefaults for an action (empty if unbound by default).
    QList<QKeySequence> defaultSequences(const QString &actionId) const;
    // edits the current set; auto-forks the built-in set first. returns true if the set changed (forked)
    bool setSequences(const QString &actionId, const QList<QKeySequence> &seqs);
    // if current set is read-only, fork to "Libreworks (custom)" with no overrides (inherits defaults) and switch
    bool ensureEditable();
    QString conflict(const QKeySequence &seq, const QString &exceptAction = {}) const;

    // true when `e` matches any sequence of `actionId` (ignores keypad distinction for Enter)
    bool matches(const QString &actionId, const QKeyEvent *e) const;
    // First non-empty shortcut (tooltip / menu / label display).
    QKeySequence firstShortcut(const QString &actionId) const;
    QString displayShortcutText(const QString &actionId) const;
    // tool.* action ids that currently include `seq` (catalog order).
    QStringList toolsSharingSequence(const QKeySequence &seq) const;

    QVector<ActionInfo> catalog() const { return m_catalog; }
    void setCatalog(QVector<ActionInfo> c) { m_catalog = std::move(c); }

    // drag-time modifier behaviours that stay as modifiers (listed in Help, not editable)
    static QVector<ModifierNote> modifierNotes();

    QString toHtml() const;

private:
    Manager() = default;
    void migrateLoadedSets(QVector<Set> &loaded, int format) const;
    static bool sameSeqs(const QList<QKeySequence> &a, const QList<QKeySequence> &b);
    QVector<Set> m_sets;
    QString m_current;
    QHash<QString, QList<QKeySequence>> m_defaults;
    QVector<ActionInfo> m_catalog;
    QHash<QString, QStringList> m_shareGroups; // tool.* cycling groups
    static constexpr int kSettingsFormat = 2;
};

} // namespace shortcuts
