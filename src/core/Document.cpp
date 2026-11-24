#include "Brand.h"
#include "PixelScale.h"
#include "Document.h"
#include "PixelEdit.h"
#include "io/Codecs.h"
#include "CorePrefs.h"
#include "Fills.h"
#include "Styles.h"
#include "Render.h"
#include "Effects.h"
#include "SelectionMask.h"
#include "Filters.h"
#include "Transform.h"
#include "io/LwdFormat.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QPainter>
#include <QLinearGradient>
#include <QtMath>
#include <algorithm>
#include <functional>

Document::Document(QObject *parent) : QObject(parent) {
    grid.x = grid.y = qMax(1, coreprefs::gridSize);
    grid.color = QColor::fromRgba(coreprefs::gridColor);
    m_hist.append({s, QStringLiteral("New"), {}, false, {}});
}

Document *Document::snapshot() const {
    auto *d = new Document;
    d->s = s;
    d->curFrame = curFrame;
    d->curLayer = curLayer;
    d->filePath = filePath;
    d->untitledName = untitledName;
    d->exportArea = exportArea;
    d->exportOptions = exportOptions;
    d->paneOptions = paneOptions;
    d->selectedPane = selectedPane;
    d->moveToThread(nullptr);   // usable and deletable from any thread
    return d;
}

Document *Document::createNew(int w, int h, const QColor &canvas, QObject *parent, double dpi) {
    auto *d = new Document(parent);
    d->s.dpi = dpi > 0 ? dpi : 72;
    d->s.width = w;
    d->s.height = h;
    d->s.canvasColor = canvas;
    LayerInfo li;
    li.id = d->newId();
    li.name = QStringLiteral("Layer 1");
    d->s.layers.append(li);
    Frame f;
    f.name = QStringLiteral("Frame 1");
    f.layers.resize(1);
    d->s.frames.append(f);
    d->resetHistory(QStringLiteral("New Document"));
    return d;
}

static void scanIds(const DocState &s, quint64 &maxId) {
    for (const auto &l : s.layers) maxId = qMax(maxId, l.id);
    for (const auto &f : s.frames)
        for (const auto &l : f.layers)
            for (const auto &o : l) o.forEachNested([&](const VObject &x) { maxId = qMax(maxId, x.id); });
    for (const auto &sym : s.symbols) {
        maxId = qMax(maxId, sym.id);
        for (const auto &f : sym.frames) for (const auto &o : f) o.forEachNested([&](const VObject &x) { maxId = qMax(maxId, x.id); });
    }
}


void Document::stampUnsetBitmaps(DocState &st) {
    auto stamp = [](VObject &top) {
        top.forEachNested([](VObject &x) { x.ensureInterpolationStamped(); });
    };
    for (Frame &f : st.frames)
        for (auto &l : f.layers)
            for (VObject &o : l) stamp(o);
    for (Symbol &sym : st.symbols)
        for (auto &fr : sym.frames)
            for (VObject &o : fr) stamp(o);
}

void Document::stampUnsetBitmaps() { stampUnsetBitmaps(s); }

void Document::resetHistory(const QString &label) {
    quint64 mx = m_nextId;
    scanIds(s, mx);
    m_nextId = mx;
    if (s.layers.isEmpty()) {
        LayerInfo li; li.id = newId(); li.name = QStringLiteral("Layer 1");
        s.layers.append(li);
    }
    if (s.frames.isEmpty()) { Frame f; f.name = QStringLiteral("Frame 1"); s.frames.append(f); }
    for (auto &f : s.frames) f.layers.resize(s.layers.size());
    curFrame = qBound(0, curFrame, s.frames.size() - 1);
    curLayer = qBound(0, curLayer, s.layers.size() - 1);
    dropPixelEdits();
    // Open/import/new: freeze Preferences Scaling onto any unset bitmaps before history snapshot.
    stampUnsetBitmaps();
    m_hist.clear();
    m_hist.append({s, label, {}, false, {}});
    m_histIndex = 0;
    m_cleanIndex = 0;
    emit historyChanged();
    notifyStructure();
}

QString Document::title() const {
    if (!filePath.isEmpty()) return QFileInfo(filePath).fileName();
    return untitledName.isEmpty() ? QStringLiteral("Untitled") : untitledName;
}

static bool stateContainersShared(const DocState &a, const DocState &b);
static QByteArray historyDigest(const DocState &st, const Document::HistEntry *hint, QVector<QByteArray> *outKeys);
static QByteArray cachedDigest(Document::HistEntry &e);

void Document::beginMerge() {
    if (m_mergeDepth++ == 0) m_mergeBase = m_histIndex;
}

void Document::endMerge(MergeEnd how) {
    if (m_mergeDepth > 0 && --m_mergeDepth == 0) {
        if (m_mergeBase >= 0 && m_histIndex == m_mergeBase + 1 && m_histIndex == m_hist.size() - 1) {
            bool drop = (how == MergeEnd::Cancel);
            if (!drop) {
                // Cheap no-op only. A real edit detaches containers; do not hash the
                // whole document (large bitmaps) on every slider release.
                drop = stateContainersShared(s, m_hist[m_mergeBase].st);
            }
            if (drop) {
                m_hist.removeLast();
                m_histIndex = m_mergeBase;
                s = m_hist[m_histIndex].st;
                emit historyChanged();
                notifyStructure();
            }
        }
        m_mergeBase = -1;
    }
}


static bool layerInfosEqual(const QVector<LayerInfo> &a, const QVector<LayerInfo> &b)
{
    if (a.size() != b.size()) return false;
    if (a.isSharedWith(b)) return true;
    for (int i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].visible != b[i].visible
            || a[i].locked != b[i].locked || a[i].shared != b[i].shared || a[i].web != b[i].web)
            return false;
    }
    return true;
}

static bool stateHeadersEqual(const DocState &a, const DocState &b)
{
    return a.width == b.width && a.height == b.height && a.canvasColor == b.canvasColor
        && a.loops == b.loops && a.dpi == b.dpi && layerInfosEqual(a.layers, b.layers);
}

// True when no object / symbol / frame-name bytes can have changed: every object
// QVector still shares with the history tip, so a no-op needs no digest.
static bool stateContainersShared(const DocState &a, const DocState &b)
{
    if (!stateHeadersEqual(a, b)) return false;
    if (a.frames.size() != b.frames.size() || a.symbols.size() != b.symbols.size()) return false;
    if (!a.symbols.isSharedWith(b.symbols)) return false;
    for (int i = 0; i < a.frames.size(); ++i) {
        if (a.frames[i].name != b.frames[i].name || a.frames[i].delayMs != b.frames[i].delayMs)
            return false;
        if (a.frames[i].layers.size() != b.frames[i].layers.size()) return false;
        for (int li = 0; li < a.frames[i].layers.size(); ++li) {
            if (!a.frames[i].layers[li].isSharedWith(b.frames[i].layers[li]))
                return false;
        }
    }
    return true;
}

static bool frameObjectsShared(const DocState &a, const DocState &b, int i)
{
    if (i < 0 || i >= a.frames.size() || i >= b.frames.size()) return false;
    if (a.frames[i].name != b.frames[i].name || a.frames[i].delayMs != b.frames[i].delayMs)
        return false;
    if (a.frames[i].layers.size() != b.frames[i].layers.size()) return false;
    for (int li = 0; li < a.frames[i].layers.size(); ++li) {
        if (!a.frames[i].layers[li].isSharedWith(b.frames[i].layers[li]))
            return false;
    }
    return true;
}

static QByteArray historyDigest(const DocState &st, const Document::HistEntry *hint, QVector<QByteArray> *outKeys)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(QByteArray::number(st.width) + 'x' + QByteArray::number(st.height));
    h.addData(st.canvasColor.name(QColor::HexArgb).toLatin1());
    h.addData(QByteArray::number(st.loops));
    h.addData(QByteArray::number(st.dpi, 'g', 17));
    for (const auto &l : st.layers) {
        h.addData(l.name.toUtf8());
        h.addData(l.shared ? "S" : "s");
        h.addData(l.web ? "W" : "w");
        h.addData(l.visible ? "V" : "v");
        h.addData(l.locked ? "L" : "l");
    }
    // Symbols once (id, name, kind, and body). contentKey used to re-hash them per frame
    // and omitted name/kind, so a rename or a kind change was treated as a no-op.
    h.addData(lwd::libraryKey(st));
    QVector<QByteArray> keys(st.frames.size());
    for (int i = 0; i < st.frames.size(); ++i) {
        h.addData(st.frames[i].name.toUtf8());
        h.addData(QByteArray::number(st.frames[i].delayMs));
        if (hint && hint->frameKeys.size() == hint->st.frames.size()
            && i < hint->frameKeys.size() && !hint->frameKeys[i].isEmpty()
            && frameObjectsShared(st, hint->st, i)
            && layerInfosEqual(st.layers, hint->st.layers)) {
            keys[i] = hint->frameKeys[i];
        } else {
            keys[i] = lwd::frameLayerKey(st, i);
        }
        h.addData(keys[i]);
    }
    if (outKeys) *outKeys = keys;
    return h.result();
}

static QByteArray cachedDigest(Document::HistEntry &e)
{
    if (e.digest.isEmpty()) e.digest = historyDigest(e.st, nullptr, &e.frameKeys);
    return e.digest;
}

void Document::commit(const QString &label, CommitMode mode) {
    endPixelEdits(true);
    syncShared();
    if (m_mergeDepth > 0 && m_histIndex == m_mergeBase + 1 && m_histIndex == m_hist.size() - 1 &&
        m_hist[m_histIndex].label == label) {
        m_hist[m_histIndex].st = s;          // fold into the step this merge already added
        // A merge fold never needs the no-op test; leave the digest lazy.
        m_hist[m_histIndex].digest = QByteArray();
        m_hist[m_histIndex].frameKeys.clear();
        trimHistory();
        validateSelection();
        emit historyChanged();
        notifyStructure();
        return;
    }
    if (mode == CommitMode::MaybeNoop && m_histIndex >= 0 && m_histIndex < m_hist.size()) {
        HistEntry &tip = m_hist[m_histIndex];
        if (stateContainersShared(s, tip.st)) {
            validateSelection();
            return;
        }
        QVector<QByteArray> keys;
        const QByteArray dig = historyDigest(s, &tip, &keys);
        if (cachedDigest(tip) == dig) {
            validateSelection();
            return;
        }
        while (m_hist.size() > m_histIndex + 1) m_hist.removeLast();
        if (m_cleanIndex > m_histIndex) m_cleanIndex = -1;
        m_hist.append({s, label, {}, false, dig, keys});
        m_histIndex = m_hist.size() - 1;
        trimHistory();
        validateSelection();
        emit historyChanged();
        notifyStructure();
        return;
    }
    while (m_hist.size() > m_histIndex + 1) m_hist.removeLast();
    if (m_cleanIndex > m_histIndex) m_cleanIndex = -1;
    m_hist.append({s, label, {}, false, {}});
    m_histIndex = m_hist.size() - 1;
    trimHistory();
    validateSelection();
    emit historyChanged();
    notifyStructure();
}

namespace {
QVector<QPair<qint64, qint64>> stateImages(const DocState &st) {
    QVector<QPair<qint64, qint64>> out;
    auto add = [&out](const QImage &im) { if (!im.isNull()) out.append({im.cacheKey(), im.sizeInBytes()}); };
    auto visit = [&add](const VObject &top) { top.forEachNested([&add](const VObject &o) { add(o.image); add(o.patternImage); }); };
    for (const Frame &f : st.frames) for (const auto &l : f.layers) for (const VObject &o : l) visit(o);
    for (const Symbol &sym : st.symbols) for (const auto &fr : sym.frames) for (const VObject &o : fr) visit(o);
    return out;
}
}

qint64 Document::historyBytes() const {
    QSet<qint64> seen;
    qint64 total = 0;
    for (const HistEntry &h : m_hist)
        for (const auto &im : h.images) if (!seen.contains(im.first)) { seen.insert(im.first); total += im.second; }
    return total;
}

void Document::trimHistory() {
    auto drop = [this](int n) {
        m_hist.remove(0, n);
        m_histIndex -= n;
        m_cleanIndex -= n;
        if (m_mergeDepth > 0) m_mergeBase -= n;
    };
    const int maxSteps = qMax(2, coreprefs::undoSteps + 1);
    if (m_hist.size() > maxSteps) drop(m_hist.size() - maxSteps);
    m_hist[m_histIndex].counted = false;   // a merge replaced its state
    for (HistEntry &h : m_hist) if (!h.counted) { h.images = stateImages(h.st); h.counted = true; }
    if (coreprefs::undoMemoryMB <= 0) return;
    // Bitmaps are shared between steps until one is edited, so an image counts once however many
    // steps hold it. Over the budget, the oldest steps go (the last step can always be undone).
    const qint64 budget = qint64(coreprefs::undoMemoryMB) << 20;
    QHash<qint64, int> refs;   // image -> number of steps holding it
    QHash<qint64, qint64> bytes;
    qint64 total = 0;
    for (const HistEntry &h : m_hist)
        for (const auto &im : h.images) {
            if (refs[im.first]++ == 0) { bytes.insert(im.first, im.second); total += im.second; }
        }
    int n = 0;
    while (total > budget && m_histIndex - n > 1 && (m_mergeDepth == 0 || n < m_mergeBase)) {
        for (const auto &im : m_hist[n].images) if (--refs[im.first] == 0) total -= bytes.value(im.first);
        ++n;
    }
    if (n > 0) drop(n);
}

void Document::restore(int index) {
    dropPixelEdits();
    m_histIndex = qBound(0, index, m_hist.size() - 1);
    s = m_hist[m_histIndex].st;
    curFrame = qBound(0, curFrame, s.frames.size() - 1);
    curLayer = qBound(0, curLayer, s.layers.size() - 1);
    validateSelection();
    emit historyChanged();
    emit frameChanged();
    notifyStructure();
}

void Document::undo() { if (canUndo()) restore(m_histIndex - 1); }
void Document::redo() { if (canRedo()) restore(m_histIndex + 1); }
void Document::jumpTo(int index) { if (index != m_histIndex) restore(index); }
void Document::revert() { dropPixelEdits(); s = m_hist[m_histIndex].st; validateSelection(); notifyStructure(); }

// ---- pixel edits (v0.3.4)
bool Document::beginPixelEdit(VObject &o) {
    if (o.type != ObjType::Bitmap) return false;
    if (!pxedit::blockingEffect(o).isEmpty()) return false;
    xf::bakeBitmap(o);
    if (!m_pixelTouched.contains(o.id)) m_pixelTouched << o.id;
    if (!m_pixelEdits.contains(o.id)) {
        pxedit::Session ses;
        if (pxedit::enter(o, ses)) m_pixelEdits.insert(o.id, ses);
    }
    return true;
}

void Document::endPixelEdits(bool trim) {
    if (m_pixelTouched.isEmpty()) return;
    const QVector<quint64> ids = m_pixelTouched;
    const QHash<quint64, pxedit::Session> sessions = m_pixelEdits;
    m_pixelTouched.clear();
    m_pixelEdits.clear();
    int baked = 0;
    for (quint64 id : ids) {
        VObject *o = find(id);
        if (!o || o->type != ObjType::Bitmap) continue;
        auto it = sessions.constFind(id);
        if (it != sessions.constEnd()) {
            const QImage display = o->image;
            const QVector<LiveEffect> zeroed = o->effects;
            bool wasBaked = false;
            pxedit::leave(*o, *it, &wasBaked);
            if (wasBaked) ++baked;
            else if (trim && o->image.hasAlphaChannel() && filters::opaqueBounds(o->image) != o->image.rect()) {
                // trimming would change the box the offset wraps in: keep what is shown, offset baked
                o->image = display;
                o->effects = zeroed;
                ++baked;
            }
        }
        if (trim) pxedit::trimToAlpha(*o);
    }
    if (baked)
        m_pixelNote = QStringLiteral("The bitmap's live Offset was applied to its pixels (its bounds changed); it looks the same");
}

void Document::dropPixelEdits() {
    m_pixelTouched.clear();
    m_pixelEdits.clear();
}

namespace {
// depth-first search through a container; fills `cont`, `index`, `parent`
bool findIn(QVector<VObject> &v, quint64 id, VObject *owner, QVector<VObject> **cont, int *index, VObject **parent) {
    for (int i = 0; i < v.size(); ++i) {
        if (v[i].id == id) { *cont = &v; *index = i; *parent = owner; return true; }
        if (v[i].type == ObjType::Group) {
            VObject *g = &v[i];
            if (findIn(g->children, id, g, cont, index, parent)) return true;
            if (findIn(g->maskObj, id, g, cont, index, parent)) return true;
        }
    }
    return false;
}
// visit selected objects in stacking order; descend only when `descend(o)`
template <class F> void walk(QVector<VObject> &v, F &&f) {
    for (VObject &o : v) {
        if (!f(o)) continue;
        if (o.type == ObjType::Group) { walk(o.children, f); walk(o.maskObj, f); }
    }
}
// remove the objects whose id is in `ids` (any depth), skipping the subtree of `keep`; prunes
// groups that lose all of their members
void removeIds(QVector<VObject> &v, const QSet<quint64> &ids, quint64 keep, bool lockedToo = true) {
    for (int i = v.size() - 1; i >= 0; --i) {
        VObject &o = v[i];
        if (o.id != keep && ids.contains(o.id) && (lockedToo || !o.locked)) { v.removeAt(i); continue; }
        if (o.type == ObjType::Group && o.id != keep) {
            const bool had = !o.children.isEmpty();
            removeIds(o.children, ids, keep, lockedToo);
            removeIds(o.maskObj, ids, keep, lockedToo);
            if (had && o.children.isEmpty()) v.removeAt(i);
        }
    }
}
}

QVector<VObject> *Document::containerOf(quint64 id, int *layerOut, int *indexOut, VObject **parentOut) {
    if (curFrame < 0 || curFrame >= s.frames.size() || id == 0) return nullptr;
    auto &f = s.frames[curFrame];
    for (int l = 0; l < f.layers.size(); ++l) {
        QVector<VObject> *cont = nullptr;
        int index = -1;
        VObject *parent = nullptr;
        if (findIn(f.layers[l], id, nullptr, &cont, &index, &parent)) {
            if (layerOut) *layerOut = l;
            if (indexOut) *indexOut = index;
            if (parentOut) *parentOut = parent;
            return cont;
        }
    }
    return nullptr;
}

VObject *Document::find(quint64 id, int *layerOut, int *indexOut) {
    int index = -1;
    QVector<VObject> *c = containerOf(id, layerOut, &index);
    if (!c) return nullptr;
    if (indexOut) *indexOut = index;
    return &(*c)[index];
}

VObject *Document::parentGroup(quint64 id) {
    VObject *parent = nullptr;
    containerOf(id, nullptr, nullptr, &parent);
    return parent;
}

VObject *Document::topLevelOf(quint64 id, int *layerOut, int *indexOut) {
    quint64 cur = id;
    while (VObject *p = parentGroup(cur)) cur = p->id;
    return find(cur, layerOut, indexOut);
}

bool Document::isMaskObject(quint64 id) {
    VObject *p = nullptr;
    QVector<VObject> *c = containerOf(id, nullptr, nullptr, &p);
    return c && p && c == &p->maskObj;
}

bool Document::isNestedIn(quint64 id, quint64 groupId) {
    for (VObject *p = parentGroup(id); p; p = parentGroup(p->id)) if (p->id == groupId) return true;
    return false;
}

void Document::enterGroup(quint64 groupId) {
    VObject *g = find(groupId);
    enteredGroup = g && g->type == ObjType::Group ? groupId : 0;
}

void Document::exitGroup() {
    VObject *p = enteredGroup ? parentGroup(enteredGroup) : nullptr;
    enteredGroup = p ? p->id : 0;
}

void Document::reassignIds(VObject &o) {
    o.forEachNested([this](VObject &x) {
        if (!x.strokeSeed)
            x.strokeSeed = quint32(x.id * 2654435761ull) ^ quint32(qHash(x.strokeKind));
        x.id = newId();
    });
}

QVector<VObject *> Document::selectedObjects() {
    QVector<VObject *> out;
    if (selection.isEmpty()) return out;
    auto &f = s.frames[curFrame];
    for (auto &l : f.layers)
        walk(l, [&](VObject &o) { if (selection.contains(o.id)) out.append(&o); return true; });
    return out;
}

QVector<VObject *> Document::selectedEditable() {
    QVector<VObject *> out;
    if (selection.isEmpty()) return out;
    auto &f = s.frames[curFrame];
    for (int li = 0; li < f.layers.size(); ++li) {
        if (s.layers[li].locked) continue;
        // a locked group locks what is inside it
        walk(f.layers[li], [&](VObject &o) {
            if (o.locked) return false;
            if (selection.contains(o.id)) out.append(&o);
            return true;
        });
    }
    return out;
}

QVector<VObject *> Document::selectedOutermost() {
    QVector<VObject *> out;
    if (selection.isEmpty()) return out;
    auto &f = s.frames[curFrame];
    for (int li = 0; li < f.layers.size(); ++li) {
        if (s.layers[li].locked) continue;
        walk(f.layers[li], [&](VObject &o) {
            if (o.locked) return false;
            if (selection.contains(o.id)) { out.append(&o); return false; }
            return true;
        });
    }
    return out;
}

VObject *Document::singleSelected() {
    if (selection.size() != 1) return nullptr;
    return find(*selection.begin());
}

void Document::addObject(VObject o, int layer, bool select) {
    if (o.isWeb()) layer = ensureWebLayer();
    else if (layer < 0 || (layer < s.layers.size() && s.layers[layer].web)) layer = drawingLayer();
    layer = qBound(0, layer, s.layers.size() - 1);
    if (o.id == 0) o.id = newId();
    // New bitmaps without a stored Scaling pick up the Preferences default once.
    o.forEachNested([](VObject &x) {
        x.ensureInterpolationStamped();
        if (!x.strokeSeed)
            x.strokeSeed = quint32(x.id * 2654435761ull) ^ quint32(qHash(x.strokeKind));
    });
    quint64 id = o.id;
    s.frames[curFrame].layers[layer].append(std::move(o));
    if (select) { curLayer = layer; selectOnly(id); }
}

void Document::deleteSelection() {
    auto &f = s.frames[curFrame];
    QSet<quint64> ids;
    for (VObject *o : selectedOutermost()) ids.insert(o->id);
    for (int li = 0; li < f.layers.size(); ++li) {
        if (s.layers[li].locked) continue;
        removeIds(f.layers[li], ids, 0, false);
    }
    validateSelection();
}

void Document::selectOnly(quint64 id) { selection = {id}; emit selectionChanged(); }
void Document::setSelection(const QSet<quint64> &sel) { selection = sel; validateSelection(); emit selectionChanged(); }
void Document::clearSelection() { if (!selection.isEmpty()) { selection.clear(); emit selectionChanged(); } }

void Document::selectAll(bool includeHotspots) {
    selection.clear();
    auto take = [&](const VObject &o) {
        return o.visible && !o.locked && (includeHotspots || o.type != ObjType::Hotspot);
    };
    if (VObject *g = enteredGroup ? find(enteredGroup) : nullptr) {
        // inside a group, Select All picks its members
        for (auto &o : g->children) if (take(o)) selection.insert(o.id);
        emit selectionChanged();
        return;
    }
    auto &f = s.frames[curFrame];
    for (int li = 0; li < f.layers.size(); ++li) {
        if (!s.layers[li].visible || s.layers[li].locked) continue;
        for (auto &o : f.layers[li]) if (take(o)) selection.insert(o.id);
    }
    emit selectionChanged();
}

void Document::validateSelection() {
    if (enteredGroup && !find(enteredGroup)) enteredGroup = 0;
    QSet<quint64> valid;
    for (quint64 id : std::as_const(selection)) if (find(id)) valid.insert(id);
    if (valid != selection) { selection = valid; emit selectionChanged(); }
}

QRectF Document::selectionBounds() {
    QRectF r;
    for (auto *o : selectedObjects()) r = r.isNull() ? o->bounds() : r.united(o->bounds());
    return r;
}

QString Document::uniqueLayerName() const {
    for (int n = s.layers.size() + 1;; ++n) {
        QString name = QStringLiteral("Layer %1").arg(n);
        bool used = false;
        for (auto &l : s.layers) used |= l.name == name;
        if (!used) return name;
    }
}

int Document::addLayer(const QString &name) {
    LayerInfo li;
    li.id = newId();
    li.name = name.isEmpty() ? uniqueLayerName() : name;
    int at = qBound(0, curLayer + 1, s.layers.size());
    int web = webLayer();
    if (web >= 0 && at > web) at = web; // the Web Layer stays on top
    s.layers.insert(at, li);
    for (auto &f : s.frames) { f.layers.resize(s.layers.size() - 1); f.layers.insert(at, {}); }
    curLayer = at;
    return at;
}

void Document::deleteLayer(int index) {
    if (s.layers.size() <= 1 || index < 0 || index >= s.layers.size()) return;
    if (!s.layers[index].web) {
        int normal = 0;
        for (const auto &l : s.layers) normal += !l.web;
        if (normal <= 1) return; // keep at least one drawing layer
    }
    s.layers.remove(index);
    for (auto &f : s.frames) if (index < f.layers.size()) f.layers.remove(index);
    curLayer = qBound(0, curLayer >= index ? curLayer - 1 : curLayer, s.layers.size() - 1);
    validateSelection();
}

void Document::moveLayer(int from, int to) {
    if (from < 0 || from >= s.layers.size() || to < 0 || to >= s.layers.size() || from == to) return;
    const int web = webLayer();
    if (web >= 0 && (from == web || to >= web)) return;   // the Web Layer stays on top
    s.layers.move(from, to);
    for (auto &f : s.frames) f.layers.move(from, to);
    if (curLayer == from) curLayer = to;
}

bool Document::moveLayers(const QVector<int> &fromIn, int destIndex)
{
    const int n = s.layers.size();
    const int web = webLayer();
    QVector<int> from;
    QSet<int> seen;
    for (int i : fromIn) {
        if (i < 0 || i >= n) continue;
        if (web >= 0 && i == web) continue;
        if (seen.contains(i)) continue;
        seen.insert(i);
        from.append(i);
    }
    if (from.isEmpty()) return false;
    std::sort(from.begin(), from.end());

    int dest = destIndex;
    if (web >= 0) dest = qBound(0, dest, web);
    else dest = qBound(0, dest, n);

    // Contiguous block dropped onto itself is a no-op.
    if (from.last() - from.first() + 1 == from.size()
        && dest >= from.first() && dest <= from.last() + 1)
        return false;

    const quint64 curId = (curLayer >= 0 && curLayer < n) ? s.layers[curLayer].id : 0;

    QVector<LayerInfo> infos;
    infos.reserve(from.size());
    QVector<QVector<QVector<VObject>>> saved(from.size());
    for (int k = 0; k < from.size(); ++k) {
        infos.append(s.layers[from[k]]);
        saved[k].resize(s.frames.size());
        for (int fi = 0; fi < s.frames.size(); ++fi)
            saved[k][fi] = s.frames[fi].layers[from[k]];
    }

    int destAdjust = 0;
    for (int li : from) if (li < dest) ++destAdjust;

    for (int i = from.size() - 1; i >= 0; --i) {
        const int li = from[i];
        s.layers.removeAt(li);
        for (auto &f : s.frames) f.layers.removeAt(li);
    }
    dest -= destAdjust;
    dest = qBound(0, dest, s.layers.size());
    const int web2 = webLayer();
    if (web2 >= 0 && dest > web2) dest = web2;

    for (int i = 0; i < infos.size(); ++i) {
        s.layers.insert(dest + i, infos[i]);
        for (int fi = 0; fi < s.frames.size(); ++fi)
            s.frames[fi].layers.insert(dest + i, saved[i][fi]);
    }

    if (curId) {
        for (int i = 0; i < s.layers.size(); ++i)
            if (s.layers[i].id == curId) { curLayer = i; break; }
    }
    return true;
}

int Document::duplicateLayer(int src) {
    if (src < 0 || src >= s.layers.size() || s.layers[src].web) return -1;
    const LayerInfo from = s.layers[src];
    curLayer = src;
    const int at = addLayer(from.name + QStringLiteral(" copy"));
    s.layers[at].visible = from.visible;
    s.layers[at].locked = from.locked;
    s.layers[at].shared = from.shared;
    QVector<VObject> sharedCopy;   // a shared layer holds the same objects (same ids) on every frame
    for (int fi = 0; fi < s.frames.size(); ++fi) {
        Frame &f = s.frames[fi];
        if (from.shared && fi > 0) { f.layers[at] = sharedCopy; continue; }
        f.layers[at] = f.layers.value(src + (src >= at ? 1 : 0));
        for (VObject &o : f.layers[at]) reassignIds(o);
        if (from.shared) sharedCopy = f.layers[at];
    }
    return at;
}


namespace {
bool isDefaultFrameName(const QString &n) {
    if (n.isEmpty()) return true;
    // "Frame 12" is the auto name; "Frame intro" is a user name and must be kept.
    if (!n.startsWith(QStringLiteral("Frame "))) return false;
    const QString rest = n.mid(6);
    if (rest.isEmpty()) return false;
    for (const QChar c : rest) if (!c.isDigit()) return false;
    return true;
}
void renumberDefaultFrameNames(QVector<Frame> &frames) {
    for (int i = 0; i < frames.size(); ++i)
        if (isDefaultFrameName(frames[i].name))
            frames[i].name = QStringLiteral("Frame %1").arg(i + 1);
}
}
void Document::addFrame(bool dup) {
    Frame f;
    if (dup) f = s.frames[curFrame];
    else f.layers.resize(s.layers.size());
    f.delayMs = s.frames[curFrame].delayMs;
    if (dup) {
        // duplicated objects need fresh ids so selection stays per-frame
        for (int li = 0; li < f.layers.size(); ++li)
            if (!s.layers.value(li).shared) for (auto &o : f.layers[li]) reassignIds(o);
    } else {
        for (int li = 0; li < f.layers.size() && li < s.layers.size(); ++li)
            if (s.layers[li].shared) f.layers[li] = s.frames[curFrame].layers.value(li);
    }
    s.frames.insert(curFrame + 1, f);
    renumberDefaultFrameNames(s.frames);
    setCurrentFrame(curFrame + 1);
}

void Document::deleteFrame(int index) {
    if (s.frames.size() <= 1 || index < 0 || index >= s.frames.size()) return;
    s.frames.remove(index);
    renumberDefaultFrameNames(s.frames);
    setCurrentFrame(qBound(0, curFrame >= index ? curFrame - 1 : curFrame, s.frames.size() - 1));
}

void Document::setCurrentFrame(int f) {
    f = qBound(0, f, s.frames.size() - 1);
    if (f == curFrame) return;
    syncShared();
    curFrame = f;
    selection.clear();
    emit selectionChanged();
    emit frameChanged();
    emit changed();
}

void Document::copyToFrames(const QVector<int> &frames)
{
    if (selection.isEmpty() || s.frames.isEmpty()) return;
    QVector<QPair<int, VObject>> items;
    for (int li = 0; li < s.layers.size(); ++li) {
        if (s.layers[li].shared || s.layers[li].web) continue;
        for (const VObject &o : objects(li))
            if (selection.contains(o.id)) items.append({li, o});
    }
    if (items.isEmpty()) return;
    for (int fi : frames) {
        if (fi < 0 || fi >= s.frames.size() || fi == curFrame) continue;
        auto &fl = s.frames[fi].layers;
        while (fl.size() < s.layers.size()) fl.append(QVector<VObject>{});
        for (auto &it : items) {
            VObject c = it.second;
            reassignIds(c);
            fl[it.first].append(c);
        }
    }
}

void Document::distributeToFrames()
{
    if (selection.isEmpty() || s.frames.isEmpty()) return;
    struct Item { int layer; quint64 id; };
    QVector<Item> items;
    for (int li = 0; li < s.layers.size(); ++li) {
        if (s.layers[li].shared || s.layers[li].web) continue;
        for (const VObject &o : objects(li))
            if (selection.contains(o.id)) items.append({li, o.id});
    }
    if (items.size() < 2) {
        // a single object is already on its own frame; still ensure nothing else to do
        return;
    }
    const int start = curFrame;
    const int need = start + items.size();
    while (s.frames.size() < need) {
        Frame f;
        f.layers.resize(s.layers.size());
        f.delayMs = s.frames.last().delayMs;
        for (int li = 0; li < s.layers.size(); ++li)
            if (s.layers[li].shared) f.layers[li] = s.frames[start].layers.value(li);
        s.frames.append(f);
    }
    renumberDefaultFrameNames(s.frames);
    for (int i = int(items.size()) - 1; i >= 1; --i) {
        auto &src = s.frames[start].layers[items[i].layer];
        for (int k = src.size() - 1; k >= 0; --k) {
            if (src[k].id != items[i].id) continue;
            VObject o = src.takeAt(k);
            auto &dst = s.frames[start + i].layers;
            while (dst.size() < s.layers.size()) dst.append(QVector<VObject>{});
            dst[items[i].layer].append(o);
            break;
        }
    }
}


void Document::resizeCanvas(int w, int h, QPoint offset) {
    s.width = qMax(1, w);
    s.height = qMax(1, h);
    if (!offset.isNull())
        for (auto &f : s.frames) for (auto &l : f.layers) for (auto &o : l) o.moveBy(offset);
}

void Document::scaleImage(int w, int h, int resample) {
    double sx = double(w) / s.width, sy = double(h) / s.height;
    std::function<void(VObject &)> scale = [&](VObject &o) {
        if (o.type == ObjType::Group) {
            for (auto &c : o.children) scale(c);
            for (auto &m : o.maskObj) scale(m);
            return;
        }
        QRectF b = o.bounds();
        QRectF nb(b.left() * sx, b.top() * sy, b.width() * sx, b.height() * sy);
        if (o.type == ObjType::Bitmap && !o.hasXform()) {
            // whole-pixel bounds, pixels 1:1 with them: no half-covered (light) edge column (v0.3.4)
            const int l = qRound(nb.left()), t = qRound(nb.top());
            nb = QRectF(l, t, qMax(1, qRound(nb.right()) - l), qMax(1, qRound(nb.bottom()) - t));
        }
        o.setBounds(nb);
        if (o.type == ObjType::Text) o.fontSize *= qMin(sx, sy);
        if (o.type == ObjType::Bitmap && !o.image.isNull()) {
            QSize ns(qMax(1, qRound(o.image.width() * sx)), qMax(1, qRound(o.image.height() * sy)));
            if (!o.hasXform() && QSizeF(o.image.size()) == b.size()) ns = nb.size().toSize();
            // Image Size: an explicit resample updates the object's Scaling; otherwise keep its own.
            const int used = resample >= 0 ? resample : o.effectiveInterpolation();
            o.image = pixelscale::resample(o.image, ns, used);
            o.interpolation = used;
        }
    };
    for (auto &f : s.frames)
        for (auto &l : f.layers)
            for (auto &o : l) scale(o);
    s.width = qMax(1, w);
    s.height = qMax(1, h);
}


QRect Document::contentPixelBounds() const {
    SymbolScope scope(&s.symbols);
    QRect out;
    for (auto &f : s.frames) {
        QVector<VObject> objs;
        QRectF pad;
        for (int li = 0; li < f.layers.size(); ++li) {
            if (s.layers.value(li).web) continue;
            for (auto &o : f.layers[li])
                if (o.visible && !o.isWeb()) { objs << o; pad = pad.isNull() ? o.visualBounds() : pad.united(o.visualBounds()); }
        }
        if (objs.isEmpty()) continue;
        const QRect area = pad.toAlignedRect().adjusted(-1, -1, 1, 1);
        if (qint64(area.width()) * area.height() > 400000000LL) { out = out.united(area); continue; }
        const QRect ob = filters::opaqueBounds(render::renderObjects(objs, area));
        if (!ob.isEmpty()) out = out.united(ob.translated(area.topLeft()));
    }
    return out;
}

void Document::arrange(int how) {
    // 0 front, 1 forward, 2 backward, 3 back; inside groups the members move within their group
    std::function<void(QVector<VObject> &)> arr = [&](QVector<VObject> &l) {
        for (auto &o : l) if (o.type == ObjType::Group) arr(o.children);
        QVector<VObject> sel, rest;
        for (auto &o : l) (selection.contains(o.id) ? sel : rest).append(o);
        if (sel.isEmpty()) return;
        if (how == 0) { l = rest + sel; return; }
        if (how == 3) { l = sel + rest; return; }
        if (how == 1) {
            for (int i = l.size() - 2; i >= 0; --i)
                if (selection.contains(l[i].id) && !selection.contains(l[i + 1].id)) std::swap(l[i], l[i + 1]);
        } else {
            for (int i = 1; i < l.size(); ++i)
                if (selection.contains(l[i].id) && !selection.contains(l[i - 1].id)) std::swap(l[i], l[i - 1]);
        }
    };
    auto &f = s.frames[curFrame];
    for (int li = 0; li < f.layers.size(); ++li) arr(f.layers[li]);
}

bool Document::relocateObjects(const QVector<quint64> &ids, int destLayer, quint64 destGroupId, int destIndex)
{
    if (ids.isEmpty()) return false;
    if (destLayer < 0 || destLayer >= s.layers.size() || destLayer >= s.frames[curFrame].layers.size())
        return false;
    if (s.layers[destLayer].web) return false;
    if (s.layers[destLayer].locked) return false;
    if (destGroupId) {
        VObject *g = find(destGroupId);
        if (!g || g->type != ObjType::Group) return false;
        if (g->locked) return false;
        for (quint64 id : ids) {
            if (id == destGroupId || isNestedIn(destGroupId, id)) return false;
            if (isMaskObject(id)) return false;
        }
    } else {
        for (quint64 id : ids)
            if (isMaskObject(id)) return false;
    }

    // Take copies in the requested order, then remove from their current containers.
    QVector<VObject> moving;
    moving.reserve(ids.size());
    QSet<quint64> seen;
    for (quint64 id : ids) {
        if (seen.contains(id)) continue;
        int layer = -1, index = -1;
        VObject *o = find(id, &layer, &index);
        if (!o) continue;
        if (s.layers.value(layer).locked || s.layers.value(layer).web) continue;
        if (o->locked) continue;
        moving.append(*o);
        seen.insert(id);
    }
    if (moving.isEmpty()) return false;

    // Record where each lives so we can detect a no-op (same parent, same index block).
    int srcLayer = -1;
    quint64 srcGroup = 0;
    int firstIndex = -1;
    bool sameParent = true;
    for (int i = 0; i < moving.size(); ++i) {
        int layer = -1, index = -1;
        VObject *parent = nullptr;
        if (!containerOf(moving[i].id, &layer, &index, &parent)) { sameParent = false; break; }
        const quint64 pg = parent ? parent->id : 0;
        if (i == 0) { srcLayer = layer; srcGroup = pg; firstIndex = index; }
        else if (layer != srcLayer || pg != srcGroup || index != firstIndex + i) sameParent = false;
    }
    if (sameParent && srcLayer == destLayer && srcGroup == destGroupId
        && destIndex >= firstIndex && destIndex <= firstIndex + moving.size())
        return false;

    for (const VObject &o : moving) {
        int layer = -1, index = -1;
        QVector<VObject> *cont = containerOf(o.id, &layer, &index);
        if (!cont || index < 0 || index >= cont->size()) return false;
    }
    // Remove, adjusting destIndex if we remove from the destination container before it.
    QVector<VObject> *dest = nullptr;
    if (destGroupId) {
        if (VObject *g = find(destGroupId)) dest = &g->children;
    } else {
        dest = &s.frames[curFrame].layers[destLayer];
    }
    if (!dest) return false;
    destIndex = qBound(0, destIndex, dest->size());

    QSet<quint64> moveSet;
    for (const VObject &o : moving) moveSet.insert(o.id);

    int destAdjust = 0;
    if (destGroupId) {
        VObject *g = find(destGroupId);
        if (g) {
            for (int i = 0; i < g->children.size() && i < destIndex; ++i)
                if (moveSet.contains(g->children[i].id)) ++destAdjust;
        }
    } else {
        auto &L = s.frames[curFrame].layers[destLayer];
        for (int i = 0; i < L.size() && i < destIndex; ++i)
            if (moveSet.contains(L[i].id)) ++destAdjust;
    }
    destIndex -= destAdjust;

    // Remove from the back of each container so indices stay valid.
    // Walk every possible container: each layer, plus groups we may nest in.
    std::function<void(QVector<VObject> &)> strip = [&](QVector<VObject> &list) {
        for (VObject &o : list)
            if (o.type == ObjType::Group) strip(o.children);
        for (int i = list.size() - 1; i >= 0; --i)
            if (moveSet.contains(list[i].id)) list.removeAt(i);
    };
    auto &f = s.frames[curFrame];
    for (int li = 0; li < f.layers.size(); ++li) strip(f.layers[li]);

    // Re-resolve dest after removals (group pointer still valid; vector may have moved).
    if (destGroupId) {
        VObject *g = find(destGroupId);
        dest = g ? &g->children : nullptr;
    } else {
        dest = &s.frames[curFrame].layers[destLayer];
    }
    if (!dest) return false;
    destIndex = qBound(0, destIndex, dest->size());
    for (int i = 0; i < moving.size(); ++i)
        dest->insert(destIndex + i, moving[i]);
    curLayer = destLayer;
    return true;
}

QImage Document::render(int frame, bool bg) const {
    return render::renderFrame(s, frame < 0 ? curFrame : frame, bg);
}

ImageSeq Document::toImageSeq(bool allFrames) const {
    ImageSeq seq;
    seq.width = s.width;
    seq.height = s.height;
    seq.loops = s.loops;
    if (allFrames) {
        for (int i = 0; i < s.frames.size(); ++i) seq.frames.append({render(i, true), s.frames[i].delayMs});
    } else {
        seq.frames.append({render(curFrame, true), s.frames[curFrame].delayMs});
    }
    return seq;
}

Document *Document::fromImageSeq(const ImageSeq &seq, QObject *parent) {
    auto *d = new Document(parent);
    d->s.width = seq.width;
    d->s.height = seq.height;
    d->s.canvasColor = Qt::transparent;
    d->s.loops = seq.loops;
    LayerInfo li; li.id = d->newId(); li.name = QStringLiteral("Layer 1");
    d->s.layers.append(li);
    for (int i = 0; i < seq.frames.size(); ++i) {
        Frame f;
        f.name = QStringLiteral("Frame %1").arg(i + 1);
        f.delayMs = seq.frames[i].delayMs > 0 ? seq.frames[i].delayMs : 100;
        f.layers.resize(1);
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("Bitmap");
        o.image = seq.frames[i].image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        o.stampInterpolation();
        o.rect = QRectF(0, 0, o.image.width(), o.image.height());
        o.fillOn = o.strokeOn = false;
        pxedit::trimToAlpha(o);   // 0.3.6: drop transparent border; canvas size stays the image size
        f.layers[0].append(o);
        d->s.frames.append(f);
    }
    d->resetHistory(QStringLiteral("Open"));
    return d;
}

Document *Document::createSample(QObject *parent) {
    Document *d = createNew(640, 420, Qt::white, parent);
    d->s.layers[0].name = QStringLiteral("Background");
    auto mk = [&](ObjType t) { VObject o; o.id = d->newId(); o.type = t; return o; };

    // background bitmap: soft gradient painted as pixels
    VObject bg = mk(ObjType::Bitmap);
    bg.name = QStringLiteral("Backdrop");
    bg.image = QImage(640, 420, QImage::Format_ARGB32_Premultiplied);
    {
        QPainter p(&bg.image);
        QLinearGradient g(0, 0, 0, 420);
        g.setColorAt(0, QColor(0xEE, 0xF3, 0xFA));
        g.setColorAt(1, QColor(0xC7, 0xD6, 0xEA));
        p.fillRect(bg.image.rect(), g);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(255, 255, 255, 150), 18, Qt::SolidLine, Qt::RoundCap));
        QPainterPath wave;
        wave.moveTo(-20, 330);
        wave.cubicTo(160, 260, 380, 420, 660, 300);
        p.drawPath(wave);
    }
    bg.stampInterpolation();
    bg.rect = QRectF(0, 0, 640, 420);
    bg.locked = false;
    d->s.frames[0].layers[0].append(bg);

    int shapes = d->addLayer(QStringLiteral("Shapes"));
    VObject r = mk(ObjType::Rect);
    r.name = QStringLiteral("Panel");
    r.rect = QRectF(40, 120, 250, 160);
    r.roundness = 20;
    r.fill = QColor(0x33, 0x66, 0x99);
    r.stroke = QColor(0x1F, 0x3D, 0x5C);
    r.strokeWidth = 2;
    r.effects.append(LiveEffect::make(EffectType::DropShadow));
    r.effects.append(LiveEffect::make(EffectType::InnerBevel));
    // a vertical linear gradient fill (editable with the Gradient tool's on-canvas handles)
    fills::setFillKind(r, FillKind::Linear);
    r.gradient = {{0.0, QColor(0x5B, 0x9B, 0xD9)}, {1.0, QColor(0x1F, 0x4E, 0x80)}};
    r.gradP0 = {0.5, 0}; r.gradP1 = {0.5, 1}; r.gradP2 = {1, 0};
    d->s.frames[0].layers[shapes].append(r);

    VObject e = mk(ObjType::Ellipse);
    e.name = QStringLiteral("Sun");
    e.rect = QRectF(360, 110, 150, 150);
    e.fill = QColor(0xFF, 0x99, 0x00);
    e.strokeOn = false;
    e.effects.append(LiveEffect::make(EffectType::OuterGlow));
    fills::setFillKind(e, FillKind::Radial);
    e.gradient = {{0.0, QColor(0xFF, 0xE8, 0x6A)}, {1.0, QColor(0xFF, 0x80, 0x00)}};
    e.gradP0 = {0.4, 0.4}; e.gradP1 = {1.0, 0.4}; e.gradP2 = {0.4, 1.0};
    d->s.frames[0].layers[shapes].append(e);

    VObject star = mk(ObjType::Path);
    star.name = QStringLiteral("Star");
    star.closed = true;
    for (int i = 0; i < 10; ++i) {
        double a = -M_PI / 2 + i * M_PI / 5;
        double rad = (i % 2) ? 24 : 55;
        star.points << QPointF(540 + rad * std::cos(a), 320 + rad * std::sin(a));
    }
    star.fill = QColor(0xFF, 0xDD, 0x33);
    star.stroke = QColor(0xCC, 0x66, 0x00);
    star.strokeWidth = 3;
    star.effects.append(LiveEffect::make(EffectType::DropShadow));
    for (const Style &st : styles::builtins())   // the built-in "Gold" style
        if (st.name == QLatin1String("Gold")) { st.applyTo(star); break; }
    d->s.frames[0].layers[shapes].append(star);

    VObject ln = mk(ObjType::Line);
    ln.name = QStringLiteral("Rule");
    ln.p1 = QPointF(40, 310);
    ln.p2 = QPointF(440, 310);
    ln.stroke = QColor(0x66, 0x66, 0x66);
    ln.strokeWidth = 3;
    d->s.frames[0].layers[shapes].append(ln);

    int texts = d->addLayer(QStringLiteral("Text"));
    VObject t = mk(ObjType::Text);
    t.text = brand::name();
    t.fontSize = 54;
    t.bold = true;
    t.fill = QColor(0x22, 0x22, 0x22);
    t.strokeOn = false;
    t.rect.moveTopLeft(QPointF(40, 30));
    t.fitTextBox();
    t.effects.append(LiveEffect::make(EffectType::DropShadow));
    t.effects[0].distance = 3;
    t.effects[0].softness = 3;
    t.effects[0].opacity = 40;
    d->s.frames[0].layers[texts].append(t);

    VObject t2 = mk(ObjType::Text);
    t2.text = QStringLiteral("native, fast, Fireworks-style");
    t2.fontSize = 18;
    t2.italic = true;
    t2.fill = QColor(0xFF, 0xFF, 0xFF);
    t2.strokeOn = false;
    t2.rect.moveTopLeft(QPointF(58, 185));
    t2.fitTextBox();
    d->s.frames[0].layers[texts].append(t2);

    // two more frames where the sun moves, for animation
    for (int fi = 1; fi < 3; ++fi) {
        Frame f = d->s.frames[0];
        f.name = QStringLiteral("Frame %1").arg(fi + 1);
        for (auto &l : f.layers) for (auto &o : l) {
            o.id = d->newId();
            if (o.name == QLatin1String("Sun")) o.moveBy(QPointF(0, -25.0 * fi));
        }
        d->s.frames.append(f);
    }
    // Web Layer: a slice over the panel and a circular hotspot on the sun
    int web = d->ensureWebLayer();
    VObject sl = mk(ObjType::Slice);
    sl.name = QStringLiteral("panel");
    sl.rect = QRectF(30, 110, 270, 180);
    sl.link = QStringLiteral("https://example.com/");
    sl.alt = brand::name() + QStringLiteral(" panel");
    d->s.frames[0].layers[web].append(sl);
    VObject hs = mk(ObjType::Hotspot);
    hs.name = QStringLiteral("sun");
    hs.webShape = WebShape::Circle;
    hs.rect = QRectF(360, 60, 150, 150);
    hs.link = QStringLiteral("#sun");
    d->s.frames[0].layers[web].append(hs);
    d->curFrame = 0;
    d->syncShared();
    d->curLayer = shapes;
    d->resetHistory(QStringLiteral("New Document"));
    return d;
}

int Document::webLayer() const {
    for (int i = 0; i < s.layers.size(); ++i) if (s.layers[i].web) return i;
    return -1;
}

int Document::ensureWebLayer() {
    int w = webLayer();
    if (w >= 0) return w;
    LayerInfo li;
    li.id = newId();
    li.name = QStringLiteral("Web Layer");
    li.shared = li.web = true;
    s.layers.append(li);
    for (auto &f : s.frames) f.layers.resize(s.layers.size());
    return s.layers.size() - 1;
}

int Document::drawingLayer() {
    curLayer = qBound(0, curLayer, s.layers.size() - 1);
    if (!s.layers[curLayer].web) return curLayer;
    for (int i = s.layers.size() - 1; i >= 0; --i)
        if (!s.layers[i].web) return curLayer = i;
    LayerInfo li;
    li.id = newId();
    li.name = QStringLiteral("Layer 1");
    s.layers.insert(0, li);
    for (auto &f : s.frames) f.layers.insert(0, {});
    return curLayer = 0;
}

void Document::setLayerShared(int index, bool shared) {
    if (index < 0 || index >= s.layers.size() || s.layers[index].web) return;
    s.layers[index].shared = shared;
    if (shared) syncShared();
}

void Document::syncShared() {
    if (curFrame < 0 || curFrame >= s.frames.size()) return;
    for (int li = 0; li < s.layers.size(); ++li) {
        if (!s.layers[li].shared) continue;
        const QVector<VObject> objs = s.frames[curFrame].layers.value(li);
        for (int fi = 0; fi < s.frames.size(); ++fi) {
            if (fi == curFrame) continue;
            if (s.frames[fi].layers.size() <= li) s.frames[fi].layers.resize(s.layers.size());
            s.frames[fi].layers[li] = objs;
        }
    }
}

// ---------------------------------------------------------------- preview pane settings
QVector<ExportOptions> Document::defaultPaneOptions(const ExportOptions &o) {
    ExportOptions a = o, b = o;
    if (ImageIO::hasQuality(o.format) && !(ImageIO::hasLossless(o.format) && o.lossless)) {
        a.quality = qMax(5, o.quality - 20);
        b.quality = qMax(5, o.quality - 40);
    } else if (o.format == Fmt::GIF || (o.format == Fmt::PNG && o.pngType == 2 && !o.tinyPng)) {
        a.colors = qMax(2, o.colors / 4);
        b.colors = qMax(2, o.colors / 16);
    } else if (o.format == Fmt::PNG && !o.pngEditable) {
        // PNG 24 / 32: compare with PNG 8 Adaptive 256 and the Tiny-compressed PNG
        a = o; a.pngType = 2; a.colors = 256; a.palette = 0; a.dither = true; a.ditherAmount = 100; a.tinyPng = false;
        b = o; b.tinyPng = codec::tinyAvailable(); if (!b.tinyPng) { b.pngType = 2; b.colors = 64; }
    } else {
        a = ExportOptions(); a.format = Fmt::WEBP; a.quality = 80; a.animated = o.animated;
        b = ExportOptions(); b.format = Fmt::JPEG; b.quality = 80;
    }
    return {o, a, b};
}

void Document::ensurePaneOptions() {
    if (paneOptions.size() == kPanes) return;
    paneOptions = defaultPaneOptions(exportOptions);
    selectedPane = 0;
}

void Document::resetPaneOptions() {
    paneOptions = defaultPaneOptions(exportOptions);
    selectedPane = 0;
}

ExportOptions Document::paneOption(int i) const {
    if (i == selectedPane || i < 0 || i >= paneOptions.size()) return exportOptions;
    return paneOptions[i];
}

void Document::selectPane(int i) {
    ensurePaneOptions();
    if (i < 0 || i >= kPanes || i == selectedPane) return;
    paneOptions[selectedPane] = exportOptions;
    selectedPane = i;
    exportOptions = paneOptions[i];
}

QVector<int> Document::onionFrames() const {
    QVector<int> r;
    const int n = int(s.frames.size());
    if (n < 2 || onionMode <= 0) return r;
    if (onionMode == 1) { if (curFrame + 1 < n) r << curFrame + 1; }
    else if (onionMode == 2) { if (curFrame > 0) r << curFrame - 1; if (curFrame + 1 < n) r << curFrame + 1; }
    else for (int i = 0; i < n; ++i) if (i != curFrame) r << i;
    return r;
}

void Document::combinePixelSelection(const QPainterPath &p, Qt::KeyboardModifiers mods, int edge, double feather) {
    const bool add = mods & Qt::ShiftModifier, sub = mods & Qt::AltModifier;
    edge = qBound(0, edge, 2);
    if ((add || sub) && hasMaskSelection()) {
        // a raster selection: combine as masks (max / a*(1-b) / min)
        const int fp = edge == SelFeather ? qCeil(feather) + 2 : 1;
        QRect area = pixelSelectionBounds().united(p.boundingRect().toAlignedRect().adjusted(-fp, -fp, fp, fp));
        const QImage a = canvasSelectionMask(area);
        QImage b = selmask::fromPath(p, area.size(), area.topLeft(), edge != SelHard);
        if (edge == SelFeather && feather > 0) b = selmask::feather(b, feather);
        setSelectionMask(add && sub ? selmask::intersect(a, b) : add ? selmask::unite(a, b) : selmask::subtract(a, b), area.topLeft());
        return;
    }
    QPainterPath r;
    if (add && sub) r = pixelSelection.intersected(p);
    else if (add) r = pixelSelection.united(p);
    else if (sub) r = pixelSelection.subtracted(p);
    else r = p;
    pixelSelection = r.simplified();
    selectionEdge = edge;
    selectionFeather = selectionEdge == SelFeather ? qMax(0.0, feather) : 0;
    emit selectionChanged();
}

QRect Document::pixelSelectionBounds() const {
    if (pixelSelection.isEmpty()) return {};
    if (hasMaskSelection()) return selmask::bounds(selMask).translated(selMaskOrigin);
    const int fp = selectionEdge == SelFeather ? qCeil(selectionFeather) + 1 : selectionEdge == SelAntiAlias ? 1 : 0;
    return pixelSelection.boundingRect().toAlignedRect().adjusted(-fp, -fp, fp, fp);
}

QImage Document::canvasSelectionMask(const QRect &area) const {
    QImage m = selmask::blank(area.size());
    if (pixelSelection.isEmpty() || area.isEmpty()) return m;
    if (hasMaskSelection()) {
        QPainter q(&m);
        q.setCompositionMode(QPainter::CompositionMode_Source);
        q.drawImage(selMaskOrigin - area.topLeft(), selMask);
        return m;
    }
    return selectionMask(QTransform::fromTranslate(-area.x(), -area.y()), area.size());
}

void Document::setSelectionMask(const QImage &maskIn, QPoint origin) {
    const QImage mask = maskIn.convertToFormat(QImage::Format_Alpha8);
    const QRect b = selmask::bounds(mask);
    if (b.isEmpty()) { clearPixelSelection(); return; }
    selMask = mask.copy(b);
    selMaskOrigin = origin + b.topLeft();
    selMaskOutline = selmask::outline(selMask, selMaskOrigin);
    if (selMaskOutline.isEmpty()) { clearPixelSelection(); return; }
    pixelSelection = selMaskOutline;
    selectionEdge = SelHard;   // the softness lives in the mask
    selectionFeather = 0;
    emit selectionChanged();
}

void Document::clearPixelSelection() {
    pixelSelection = QPainterPath();
    selMask = QImage();
    selMaskOutline = QPainterPath();
    emit selectionChanged();
}

bool Document::selectionContains(const QPointF &p) const {
    if (pixelSelection.isEmpty()) return false;
    if (hasMaskSelection()) {
        const QPoint ip = QPoint(qFloor(p.x()), qFloor(p.y())) - selMaskOrigin;
        return selMask.rect().contains(ip) && selMask.constScanLine(ip.y())[ip.x()] >= 128;
    }
    return pixelSelection.contains(p);
}

void Document::moveSelection(const QPointF &by) {
    if (pixelSelection.isEmpty() || by.isNull()) return;
    if (hasMaskSelection()) {
        const QPoint d(qRound(by.x()), qRound(by.y()));
        selMaskOrigin += d;
        selMaskOutline.translate(d);
        pixelSelection = selMaskOutline;
    } else {
        pixelSelection.translate(by);
    }
    emit selectionChanged();
}

QImage Document::selectionMask(const QTransform &docToImg, QSize size) const {
    if (pixelSelection.isEmpty() || size.isEmpty()) return {};
    QImage m(size, QImage::Format_Alpha8);
    m.fill(0);
    if (hasMaskSelection()) {
        const QTransform t = QTransform::fromTranslate(selMaskOrigin.x(), selMaskOrigin.y()) * docToImg;
        QPainter p(&m);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        if (t.type() <= QTransform::TxTranslate && qFuzzyCompare(t.dx(), std::round(t.dx())) && qFuzzyCompare(t.dy() + 1, std::round(t.dy()) + 1)) {
            p.drawImage(QPoint(qRound(t.dx()), qRound(t.dy())), selMask);   // exact
        } else {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.setTransform(t);
            p.drawImage(0, 0, selMask);
        }
        return m;
    }
    {
        QPainter p(&m);
        p.setRenderHint(QPainter::Antialiasing, selectionEdge != SelHard);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.drawPath(docToImg.map(pixelSelection));
    }
    if (selectionEdge == SelFeather && selectionFeather > 0) {
        const double scale = std::sqrt(qAbs(docToImg.determinant()));
        fx::blur(m, selectionFeather * scale / 2.0);
    }
    return m;
}
