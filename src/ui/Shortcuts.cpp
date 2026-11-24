#include "Shortcuts.h"
#include "Brand.h"
#include <QAction>
#include <QDateTime>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMetaObject>
#include <QSet>
#include <QSettings>
#include <QTextStream>
#include <algorithm>

namespace shortcuts {

Manager &Manager::get() {
    static Manager m;
    return m;
}

void Manager::captureDefaults(QMainWindow *win) {
    if (!win) return;
    // (re)capture so late-registered canvas.* actions are included
    m_shareGroups.clear();
    for (QAction *a : win->findChildren<QAction *>()) {
        if (a->objectName().isEmpty()) continue;
        QList<QKeySequence> seqs = a->shortcuts();
        if (seqs.isEmpty() && !a->shortcut().isEmpty()) seqs = {a->shortcut()};
        if (!seqs.isEmpty()) m_defaults.insert(a->objectName(), seqs);
        else if (!m_defaults.contains(a->objectName())) m_defaults.insert(a->objectName(), {});
        const QStringList share = a->property("lwToolShareGroup").toStringList();
        if (share.size() > 1) m_shareGroups.insert(a->objectName(), share);
    }
    if (m_sets.isEmpty()) {
        Set def;
        def.id = brand::nameLower();
        def.name = brand::name();
        def.readOnly = true;
        def.keys = m_defaults;
        m_sets.append(def);
        m_current = def.id;
    } else {
        // keep built-in defaults in sync with newly registered actions
        for (Set &s : m_sets) if (s.id == brand::nameLower()) {
            for (auto it = m_defaults.begin(); it != m_defaults.end(); ++it)
                if (!s.keys.contains(it.key())) s.keys.insert(it.key(), it.value());
        }
    }
}

const Set *Manager::set(const QString &id) const {
    for (const Set &s : m_sets) if (s.id == id) return &s;
    return nullptr;
}
Set *Manager::set(const QString &id) {
    for (Set &s : m_sets) if (s.id == id) return &s;
    return nullptr;
}

void Manager::setCurrent(const QString &id) {
    if (set(id)) m_current = id;
}

QString Manager::duplicate(const QString &id, const QString &newName) {
    const Set *src = set(id);
    if (!src) return {};
    Set n = *src;
    n.id = QStringLiteral("user-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(m_sets.size());
    n.name = newName.isEmpty() ? src->name + QStringLiteral(" (custom)") : newName;
    n.readOnly = false;
    // start from full defaults merged with overrides
    for (auto it = m_defaults.begin(); it != m_defaults.end(); ++it)
        if (!n.keys.contains(it.key())) n.keys.insert(it.key(), it.value());
    m_sets.append(n);
    m_current = n.id;
    return n.id;
}

bool Manager::rename(const QString &id, const QString &newName) {
    Set *s = set(id);
    if (!s || s->readOnly || newName.trimmed().isEmpty()) return false;
    for (const Set &o : m_sets) if (o.id != id && o.name == newName) return false;
    s->name = newName.trimmed();
    return true;
}

bool Manager::remove(const QString &id) {
    Set *s = set(id);
    if (!s || s->readOnly) return false;
    for (int i = 0; i < m_sets.size(); ++i) if (m_sets[i].id == id) {
        m_sets.removeAt(i);
        if (m_current == id) m_current = brand::nameLower();
        return true;
    }
    return false;
}

QList<QKeySequence> Manager::sequences(const QString &actionId) const {
    const Set *s = set(m_current);
    if (!s) return m_defaults.value(actionId);
    // Built-in set always reflects live captureDefaults (never a stale/empty saved copy).
    if (s->readOnly) return m_defaults.value(actionId);
    // Format 2: only overrides live in keys. Missing = inherit; present empty = unbound.
    if (s->keys.contains(actionId)) return s->keys.value(actionId);
    return m_defaults.value(actionId);
}

QList<QKeySequence> Manager::defaultSequences(const QString &actionId) const {
    return m_defaults.value(actionId);
}

bool Manager::sameSeqs(const QList<QKeySequence> &a, const QList<QKeySequence> &b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

static QString uniqueCustomName(const QVector<Set> &sets, const QString &baseName) {
    const QString first = baseName + QStringLiteral(" (custom)");
    auto taken = [&](const QString &n) {
        for (const Set &o : sets) if (o.name == n) return true;
        return false;
    };
    if (!taken(first)) return first;
    for (int i = 2; i < 1000; ++i) {
        const QString n = baseName + QStringLiteral(" (custom %1)").arg(i);
        if (!taken(n)) return n;
    }
    return baseName + QStringLiteral(" (custom %1)").arg(QDateTime::currentMSecsSinceEpoch());
}

bool Manager::ensureEditable() {
    Set *s = set(m_current);
    if (!s) return false;
    if (!s->readOnly) return false;
    // Auto-fork with no overrides so Tools/Liquify/panels/nudge inherit live defaults until edited.
    const QString name = uniqueCustomName(m_sets, s->name);
    Set n;
    n.id = QStringLiteral("user-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(m_sets.size());
    n.name = name;
    n.readOnly = false;
    m_sets.append(n);
    m_current = n.id;
    return true;
}

bool Manager::setSequences(const QString &actionId, const QList<QKeySequence> &seqs) {
    const bool forked = ensureEditable();
    Set *s = set(m_current);
    if (!s || s->readOnly) return forked;
    // Explicit empty = deliberate unbind (stored). Matching defaults = drop override (inherit).
    // 0.3.11+: flyout mates are NOT force-synced — each tool keeps its own sequences after rebind.
    if (seqs.isEmpty()) {
        s->keys.insert(actionId, {});
    } else if (sameSeqs(seqs, m_defaults.value(actionId))) {
        s->keys.remove(actionId);
    } else {
        s->keys.insert(actionId, seqs);
    }
    return forked;
}

QString Manager::conflict(const QKeySequence &seq, const QString &exceptAction) const {
    if (seq.isEmpty()) return {};
    const Set *s = set(m_current);
    // Tools that share a key by choice are allowed to collide (they cycle). Only report
    // conflicts outside the excepted action; caller decides whether to reassign.
    const bool exceptIsTool = exceptAction.startsWith(QLatin1String("tool."));
    const bool exceptIsLiquify = exceptAction.startsWith(QLatin1String("liquify."));
    const bool exceptIsPanelRename = exceptAction == QLatin1String("panels.rename");
    auto ignored = [&](const QString &id) {
        if (id == exceptAction) return true;
        // tool ↔ tool sharing is intentional (Fireworks-style cycling)
        if (exceptIsTool && id.startsWith(QLatin1String("tool."))) return true;
        // Liquify keys are dialog-local and may reuse tool letters
        if (exceptIsTool && id.startsWith(QLatin1String("liquify."))) return true;
        if (exceptIsLiquify && (id.startsWith(QLatin1String("tool.")) || id.startsWith(QLatin1String("liquify."))))
            return true;
        // F2: rename-when-focused vs Window > Layers toggle (context-specific)
        if (exceptIsPanelRename && id.startsWith(QLatin1String("window."))) return true;
        if (exceptAction.startsWith(QLatin1String("window.")) && id == QLatin1String("panels.rename")) return true;
        return false;
    };
    if (s) {
        QSet<QString> seen;
        for (auto it = s->keys.begin(); it != s->keys.end(); ++it) {
            seen.insert(it.key());
            if (ignored(it.key())) continue;
            for (const QKeySequence &k : it.value())
                if (k == seq) return it.key();
        }
        for (auto it = m_defaults.begin(); it != m_defaults.end(); ++it) {
            if (seen.contains(it.key()) || ignored(it.key())) continue;
            for (const QKeySequence &k : it.value())
                if (k == seq) return it.key();
        }
        return {};
    }
    for (auto it = m_defaults.begin(); it != m_defaults.end(); ++it) {
        if (ignored(it.key())) continue;
        for (const QKeySequence &k : it.value())
            if (k == seq) return it.key();
    }
    return {};
}

bool Manager::matches(const QString &actionId, const QKeyEvent *e) const {
    if (!e) return false;
    const auto mods = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    auto sameKey = [](int a, int b) {
        if (a == b) return true;
        // +/-/=/_ and Enter/Return are interchangeable on keyboards / keypad
        const bool aMinus = a == Qt::Key_Minus || a == Qt::Key_Underscore;
        const bool bMinus = b == Qt::Key_Minus || b == Qt::Key_Underscore;
        if (aMinus && bMinus) return true;
        const bool aPlus = a == Qt::Key_Plus || a == Qt::Key_Equal;
        const bool bPlus = b == Qt::Key_Plus || b == Qt::Key_Equal;
        if (aPlus && bPlus) return true;
        const bool aEnter = a == Qt::Key_Return || a == Qt::Key_Enter;
        const bool bEnter = b == Qt::Key_Return || b == Qt::Key_Enter;
        return aEnter && bEnter;
    };
    // Build a keypad-stripped combination for ExactMatch against registered sequences
    const QKeySequence got(QKeyCombination(mods, Qt::Key(e->key())));
    for (const QKeySequence &k : sequences(actionId)) {
        if (k.isEmpty() || k.count() < 1) continue;
        if (!got.isEmpty() && (k == got || k.matches(got) == QKeySequence::ExactMatch)) return true;
        if (sameKey(int(k[0].key()), e->key()) && k[0].keyboardModifiers() == mods) return true;
    }
    return false;
}

QKeySequence Manager::firstShortcut(const QString &actionId) const {
    const QList<QKeySequence> seqs = sequences(actionId);
    for (const QKeySequence &k : seqs)
        if (!k.isEmpty()) return k;
    return {};
}

QStringList Manager::toolsSharingSequence(const QKeySequence &seq) const {
    QStringList out;
    if (seq.isEmpty()) return out;
    QSet<QString> seen;
    auto consider = [&](const QString &id) {
        if (!id.startsWith(QLatin1String("tool.")) || seen.contains(id)) return;
        for (const QKeySequence &k : sequences(id))
            if (k == seq) { out << id; seen.insert(id); return; }
    };
    for (const ActionInfo &a : m_catalog) consider(a.id);
    for (auto it = m_defaults.begin(); it != m_defaults.end(); ++it) consider(it.key());
    return out;
}

QString Manager::displayShortcutText(const QString &actionId) const {
    const QKeySequence k = firstShortcut(actionId);
    return k.isEmpty() ? QString() : k.toString(QKeySequence::NativeText);
}

QVector<ModifierNote> Manager::modifierNotes() {
    return {
        {QStringLiteral("While dragging a fill handle"), QStringLiteral("Shift"), QStringLiteral("Scale all handles together about the centre")},
        {QStringLiteral("While dragging a fill handle"), QStringLiteral("Ctrl"), QStringLiteral("Snap angle to 45° (pattern bar turn: 15°)")},
        {QStringLiteral("While drawing / transforming"), QStringLiteral("Shift"), QStringLiteral("Constrain proportions or angle")},
        {QStringLiteral("While drawing a shape or selection"), QStringLiteral("Alt"), QStringLiteral("Draw from the centre")},
        {QStringLiteral("While dragging objects"), QStringLiteral("Ctrl"), QStringLiteral("Temporarily turn off smart snapping")},
        {QStringLiteral("While dragging a pop-up slider"), QStringLiteral("Shift"), QStringLiteral("Fine control (Preferences speed)")},
        {QStringLiteral("While dragging or stepping a slider"), QStringLiteral("Ctrl"), QStringLiteral("Snap to Preferences increment")},
        {QStringLiteral("Marquee / Lasso / Wand"), QStringLiteral("Shift / Alt"), QStringLiteral("Add to / subtract from the pixel selection")},
        {QStringLiteral("Eyedropper click"), QStringLiteral("Shift"), QStringLiteral("Set stroke colour instead of fill")},
        {QStringLiteral("Stamp / Healing"), QStringLiteral("Alt"), QStringLiteral("Pick the source point")},
    };
}

void Manager::apply(QMainWindow *win) const {
    if (!win) return;
    // Per unique tool shortcut: only the first tool (catalog order) that owns that exact
    // sequence gets WindowShortcut so Qt fires once; its handler cycles among all tools that
    // currently share the sequence. Tools with a unique key always fire themselves.
    QHash<QString, QString> seqOwner; // portable seq string -> first tool id
    for (const ActionInfo &info : m_catalog) {
        if (!info.id.startsWith(QLatin1String("tool."))) continue;
        for (const QKeySequence &k : sequences(info.id)) {
            if (k.isEmpty()) continue;
            const QString key = k.toString(QKeySequence::PortableText);
            if (!seqOwner.contains(key)) seqOwner.insert(key, info.id);
        }
    }
    for (QAction *a : win->findChildren<QAction *>()) {
        if (a->objectName().isEmpty()) continue;
        const QList<QKeySequence> seqs = sequences(a->objectName());
        a->setShortcuts(seqs); // order preserved: Qt menus/tooltips use the first
        if (a->objectName().startsWith(QLatin1String("canvas."))) {
            a->setShortcutContext(Qt::WidgetShortcut);
            continue;
        }
        if (a->objectName().startsWith(QLatin1String("tool."))) {
            static const QSet<QString> kTipKeys{QStringLiteral("tool.sizedown"), QStringLiteral("tool.sizeup"),
                                                QStringLiteral("tool.softdown"), QStringLiteral("tool.softup")};
            if (kTipKeys.contains(a->objectName())) {
                // MainWindow::event deliberately leaves tip size/softness to QAction; WindowShortcut
                // fires while the canvas is focused, and auto-repeat still grows the tip.
                a->setShortcutContext(Qt::WindowShortcut);
                continue;
            }
            bool owner = false;
            for (const QKeySequence &k : seqs) {
                if (k.isEmpty()) continue;
                if (seqOwner.value(k.toString(QKeySequence::PortableText)) == a->objectName())
                    owner = true;
            }
            // WidgetShortcut: do not let QAction steal keys; MainWindow::event dispatches tools.
            a->setShortcutContext(Qt::WidgetShortcut);
            a->setProperty("lwToolShortcutOwner", owner);
            continue;
        }
        // setShortcuts() resets context to WindowShortcut — keep dialog/panel-local keys quiet
        if (a->objectName().startsWith(QLatin1String("liquify."))
            || a->objectName().startsWith(QLatin1String("panels."))) {
            a->setShortcutContext(Qt::WidgetShortcut);
            continue;
        }
    }
    QMetaObject::invokeMethod(win, "refreshToolShortcutLabels", Qt::DirectConnection);
}

void Manager::migrateLoadedSets(QVector<Set> &loaded, int format) const {
    for (Set &s : loaded) {
        if (s.id == brand::nameLower()) {
            s.readOnly = true;
            s.name = brand::name();
            s.keys = m_defaults; // display cache only; sequences() ignores keys for read-only
            continue;
        }
        if (format >= kSettingsFormat) continue;
        // Format 1 (0.3.6–0.3.12): custom sets often stored explicit empty lists for actions
        // that were not yet in the registry (or were captured before they had defaults). Those
        // empties are indistinguishable from a deliberate unbind in the old marker, but they
        // blank the Preferences list while the construction-time QAction keys still "work".
        // Drop empty entries when live defaults are non-empty so the action inherits again.
        QStringList drop;
        for (auto it = s.keys.constBegin(); it != s.keys.constEnd(); ++it) {
            if (!it.value().isEmpty()) continue;
            if (!m_defaults.value(it.key()).isEmpty()) drop.append(it.key());
        }
        for (const QString &id : drop) s.keys.remove(id);
        // Drop overrides that merely restate current defaults (sparse format 2).
        drop.clear();
        for (auto it = s.keys.constBegin(); it != s.keys.constEnd(); ++it) {
            if (it.value().isEmpty()) continue;
            if (sameSeqs(it.value(), m_defaults.value(it.key()))) drop.append(it.key());
        }
        for (const QString &id : drop) s.keys.remove(id);
    }
}

void Manager::load(QSettings &st) {
    st.beginGroup(QStringLiteral("shortcuts"));
    const int format = st.value(QStringLiteral("format"), 1).toInt();
    m_current = st.value(QStringLiteral("current"), brand::nameLower()).toString();
    const int n = st.beginReadArray(QStringLiteral("sets"));
    QVector<Set> loaded;
    for (int i = 0; i < n; ++i) {
        st.setArrayIndex(i);
        Set s;
        s.id = st.value(QStringLiteral("id")).toString();
        s.name = st.value(QStringLiteral("name")).toString();
        s.readOnly = st.value(QStringLiteral("readOnly"), false).toBool();
        const int m = st.beginReadArray(QStringLiteral("keys"));
        for (int j = 0; j < m; ++j) {
            st.setArrayIndex(j);
            const QString id = st.value(QStringLiteral("id")).toString();
            if (id.isEmpty()) continue;
            QStringList parts = st.value(QStringLiteral("seq")).toStringList();
            // Explicit unbind marker: a single empty string (format 1 and 2).
            if (parts.size() == 1 && parts[0].isEmpty()) {
                s.keys.insert(id, {});
                continue;
            }
            QList<QKeySequence> seqs;
            for (const QString &p : parts) {
                if (p.isEmpty()) continue;
                QKeySequence k(p);
                if (!k.isEmpty()) seqs << k;
            }
            // Empty parts with no marker: treat as stale "never saved" (do not insert).
            if (!seqs.isEmpty()) s.keys.insert(id, seqs);
        }
        st.endArray();
        if (!s.id.isEmpty()) loaded.append(s);
    }
    st.endArray();
    st.endGroup();

    bool hasDef = false;
    for (const Set &s : loaded) if (s.id == brand::nameLower()) hasDef = true;
    if (!hasDef) {
        Set def;
        def.id = brand::nameLower();
        def.name = brand::name();
        def.readOnly = true;
        def.keys = m_defaults;
        loaded.prepend(def);
    }
    migrateLoadedSets(loaded, format);
    if (!loaded.isEmpty()) m_sets = loaded;
    if (!set(m_current)) m_current = brand::nameLower();
}

void Manager::save(QSettings &st) const {
    st.beginGroup(QStringLiteral("shortcuts"));
    st.setValue(QStringLiteral("format"), kSettingsFormat);
    st.setValue(QStringLiteral("current"), m_current);
    st.beginWriteArray(QStringLiteral("sets"), m_sets.size());
    for (int i = 0; i < m_sets.size(); ++i) {
        st.setArrayIndex(i);
        const Set &s = m_sets[i];
        st.setValue(QStringLiteral("id"), s.id);
        st.setValue(QStringLiteral("name"), s.name);
        st.setValue(QStringLiteral("readOnly"), s.readOnly);
        // Built-in: do not persist keys (always live defaults). Custom: only overrides.
        QList<QString> ids;
        if (!s.readOnly) {
            for (auto it = s.keys.constBegin(); it != s.keys.constEnd(); ++it) {
                if (it.value().isEmpty()) {
                    ids.append(it.key()); // deliberate unbind
                    continue;
                }
                if (!sameSeqs(it.value(), m_defaults.value(it.key())))
                    ids.append(it.key());
            }
        }
        std::sort(ids.begin(), ids.end());
        st.beginWriteArray(QStringLiteral("keys"), ids.size());
        for (int j = 0; j < ids.size(); ++j) {
            st.setArrayIndex(j);
            const QString id = ids[j];
            st.setValue(QStringLiteral("id"), id);
            QStringList parts;
            for (const QKeySequence &k : s.keys.value(id))
                parts << k.toString(QKeySequence::PortableText);
            if (parts.isEmpty()) parts << QString(); // unbound marker
            st.setValue(QStringLiteral("seq"), parts);
        }
        st.endArray();
    }
    st.endArray();
    st.endGroup();
}

QString Manager::toHtml() const {
    QString html;
    QTextStream o(&html);
    o << "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" << brand::name().toHtmlEscaped() << " Keyboard Shortcuts</title>\n"
         "<style>body{font:14px/1.45 system-ui,sans-serif;margin:2rem;color:#222}"
         "h1{font-size:1.4rem}h2{font-size:1.1rem;margin-top:1.6rem;border-bottom:1px solid #ccc;padding-bottom:.3rem}"
         "table{border-collapse:collapse;width:100%;max-width:720px}th,td{text-align:left;padding:.35rem .5rem;border-bottom:1px solid #eee}"
         "th{background:#f5f5f5}td.k{font-family:ui-monospace,monospace;white-space:nowrap}"
         ".note{color:#555;font-size:.92rem;margin:.4rem 0 1rem}</style></head><body>\n";
    const Set *s = set(m_current);
    o << "<h1>" << brand::name().toHtmlEscaped() << " Keyboard Shortcuts</h1><p>Set: <b>" << (s ? s->name : brand::name()) << "</b></p>\n";
    auto dump = [&](const QString &title, const QString &cat) {
        o << "<h2>" << title << "</h2><table><tr><th>Command</th><th>Shortcut(s)</th></tr>\n";
        for (const ActionInfo &a : m_catalog) {
            if (a.category != cat) continue;
            const QList<QKeySequence> seqs = sequences(a.id);
            QStringList parts;
            for (const QKeySequence &k : seqs) parts << k.toString(QKeySequence::NativeText);
            o << "<tr><td>" << a.label.toHtmlEscaped() << "</td><td class=\"k\">"
              << (parts.isEmpty() ? QStringLiteral("(none)") : parts.join(QStringLiteral(", ")).toHtmlEscaped())
              << "</td></tr>\n";
        }
        o << "</table>\n";
    };
    dump(QStringLiteral("Menu Commands"), QStringLiteral("menu"));
    dump(QStringLiteral("Tools"), QStringLiteral("tools"));
    dump(QStringLiteral("Miscellaneous"), QStringLiteral("misc"));
    o << "<h2>Modifiers</h2>"
         "<p class=\"note\">These stay as modifiers held during a drag or click. They are not shortcut bindings and cannot be remapped.</p>"
         "<table><tr><th>When</th><th>Modifier</th><th>Effect</th></tr>\n";
    for (const ModifierNote &n : modifierNotes())
        o << "<tr><td>" << n.when.toHtmlEscaped() << "</td><td class=\"k\">" << n.keys.toHtmlEscaped()
          << "</td><td>" << n.effect.toHtmlEscaped() << "</td></tr>\n";
    o << "</table>\n</body></html>\n";
    return html;
}

} // namespace shortcuts
