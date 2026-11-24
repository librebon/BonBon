// Document symbol operations (Library panel, Convert to Symbol, Tween Instances, ...).
#include "Document.h"
#include "Render.h"
#include <algorithm>

namespace {
// Maps an object's geometry into `nb` (its new bounds), scaling text with it.
void placeObject(VObject &o, const QRectF &nb) {
    const QRectF ob = o.bounds();
    const double sy = ob.height() > 1e-9 ? nb.height() / ob.height() : 1.0;
    o.setBounds(nb);
    if (o.type == ObjType::Text) o.fontSize = qMax(1.0, o.fontSize * sy);
}
QRectF lerpRect(const QRectF &a, const QRectF &b, double t) {
    return QRectF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t,
                  a.width() + (b.width() - a.width()) * t, a.height() + (b.height() - a.height()) * t);
}
}

Symbol *Document::symbol(quint64 id) {
    for (Symbol &sym : s.symbols) if (sym.id == id) return &sym;
    return nullptr;
}
const Symbol *Document::symbol(quint64 id) const {
    for (const Symbol &sym : s.symbols) if (sym.id == id) return &sym;
    return nullptr;
}

QString Document::uniqueSymbolName(const QString &base0) const {
    const QString base = base0.trimmed().isEmpty() ? QStringLiteral("Symbol") : base0.trimmed();
    auto taken = [&](const QString &n) { for (const Symbol &sym : s.symbols) if (sym.name == n) return true; return false; };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) if (!taken(QStringLiteral("%1 %2").arg(base).arg(i))) return QStringLiteral("%1 %2").arg(base).arg(i);
}

quint64 Document::addSymbol(Symbol sym) {
    sym.id = newId();
    sym.name = uniqueSymbolName(sym.name);
    if (sym.frames.isEmpty()) sym.frames.resize(1);
    if (sym.kind == SymbolKind::Button) {
        while (sym.frames.size() < 4) sym.frames.append(sym.frames.first());
        sym.frames.resize(4);
    }
    if (sym.kind == SymbolKind::Graphic) sym.frames.resize(1);
    const quint64 id = sym.id;
    s.symbols.append(std::move(sym));
    return id;
}

VObject Document::makeInstance(quint64 symId) const {
    VObject o;
    const Symbol *sym = symbol(symId);
    if (!sym) return o;
    o.type = ObjType::Instance;
    o.symbolId = symId;
    o.name = sym->name;
    o.rect = sym->bounds();
    o.fillOn = o.strokeOn = false;
    return o;
}

quint64 Document::convertToSymbol(const QString &name, SymbolKind kind) {
    auto &f = s.frames[curFrame];
    QVector<VObject> objs;
    int topLayer = -1, topIndex = -1;
    for (int li = 0; li < f.layers.size(); ++li) {
        if (s.layers[li].locked || s.layers[li].web) continue;
        for (int i = 0; i < f.layers[li].size(); ++i) {
            const VObject &o = f.layers[li][i];
            if (!selection.contains(o.id) || o.locked || o.isWeb()) continue;
            objs.append(o);
            topLayer = li;
            topIndex = i;
        }
    }
    if (objs.isEmpty()) return 0;
    Symbol sym;
    sym.name = name;
    sym.kind = kind;
    sym.frames = {objs};
    const quint64 id = addSymbol(sym);
    // remove the originals, remembering where the topmost one was
    int insertAt = topIndex;
    for (int li = 0; li < f.layers.size(); ++li) {
        auto &l = f.layers[li];
        for (int i = l.size() - 1; i >= 0; --i) {
            const VObject &o = l[i];
            if (!selection.contains(o.id) || o.locked || o.isWeb() || s.layers[li].locked || s.layers[li].web) continue;
            if (li == topLayer && i < topIndex) --insertAt;
            l.remove(i);
        }
    }
    VObject inst = makeInstance(id);
    inst.id = newId();
    auto &tl = f.layers[topLayer];
    tl.insert(qBound(0, insertAt, int(tl.size())), inst);
    selection = {inst.id};
    emit selectionChanged();
    return id;
}

int Document::appendFrame() {
    Frame nf;
    nf.layers.resize(s.layers.size());
    nf.delayMs = s.frames.last().delayMs;
    for (int li = 0; li < s.layers.size(); ++li)
        if (s.layers[li].shared) nf.layers[li] = s.frames[curFrame].layers.value(li);
    s.frames.append(nf);
    for (int i = 0; i < s.frames.size(); ++i) s.frames[i].name = QStringLiteral("Frame %1").arg(i + 1);
    return s.frames.size() - 1;
}

QVector<quint64> Document::insertInstance(quint64 symId, const QPointF &center) {
    QVector<quint64> ids;
    const Symbol *sym = symbol(symId);
    if (!sym) return ids;
    VObject inst = makeInstance(symId);
    QRectF r = inst.rect;
    r.moveCenter(center);
    inst.rect = r;
    const int layer = drawingLayer();
    const int n = sym->kind == SymbolKind::Animation ? sym->frameCount() : 1;
    for (int k = 0; k < n; ++k) {
        const int fi = curFrame + k;
        while (fi >= s.frames.size()) appendFrame();
        VObject o = inst;
        reassignIds(o);
        o.symFrame = k;
        s.frames[fi].layers[layer].append(o);
        ids << o.id;
    }
    selection = {ids.first()};
    emit selectionChanged();
    return ids;
}

void Document::setSymbolFrames(quint64 symId, const QVector<QVector<VObject>> &frames) {
    render::clearObjectVisualCache();
    // Digests may nest; drop every symbol's cache so outer instances rehash (0.3.17).
    for (Symbol &symAll : this->s.symbols) symAll.invalidateBodyKey();
    Symbol *sym = symbol(symId);
    if (!sym) return;
    const QRectF ob = sym->bounds();
    sym->frames = frames.isEmpty() ? QVector<QVector<VObject>>(1) : frames;
    if (sym->kind == SymbolKind::Graphic) sym->frames.resize(1);
    if (sym->kind == SymbolKind::Button) {
        while (sym->frames.size() < 4) sym->frames.append(sym->frames.first());
        sym->frames.resize(4);
    }
    const QRectF nb = sym->bounds();
    const int nf = sym->frameCount();
    auto fix = [&](VObject &o) {
        if (o.type != ObjType::Instance || o.symbolId != symId) return;
        o.symFrame = qBound(0, o.symFrame, nf - 1);
        if (ob == nb) return;
        const QRectF r = o.rect.normalized();
        if (ob.width() <= 0 || ob.height() <= 0 || r.width() <= 0 || r.height() <= 0) {
            o.rect = QRectF(QPointF(0, 0), nb.size());
            o.rect.moveCenter(r.center());
            return;
        }
        const double sx = r.width() / ob.width(), sy = r.height() / ob.height();
        o.rect = QRectF(r.x() + (nb.x() - ob.x()) * sx, r.y() + (nb.y() - ob.y()) * sy, nb.width() * sx, nb.height() * sy);
    };
    for (auto &f : s.frames) for (auto &l : f.layers) for (auto &o : l) o.forEachNested(fix);
    for (auto &other : s.symbols) for (auto &f : other.frames) for (auto &o : f) o.forEachNested(fix);
}

bool Document::deleteSymbol(quint64 symId) {
    render::clearObjectVisualCache();
    for (Symbol &symAll : s.symbols) symAll.invalidateBodyKey();
    const int before = s.symbols.size();
    s.symbols.erase(std::remove_if(s.symbols.begin(), s.symbols.end(), [&](const Symbol &x) { return x.id == symId; }), s.symbols.end());
    if (s.symbols.size() == before) return false;
    auto eraseInstances = [&](auto &&self, QVector<VObject> &objects) -> void {
        objects.erase(std::remove_if(objects.begin(), objects.end(), [&](VObject &o) {
            if (o.type == ObjType::Instance && o.symbolId == symId) return true;
            self(self, o.children);
            self(self, o.maskObj);
            return false;
        }), objects.end());
    };
    for (auto &f : s.frames) for (auto &l : f.layers) eraseInstances(eraseInstances, l);
    for (auto &other : s.symbols) for (auto &f : other.frames) eraseInstances(eraseInstances, f);
    validateSelection();
    return true;
}

quint64 Document::duplicateSymbol(quint64 symId) {
    const Symbol *sym = symbol(symId);
    if (!sym) return 0;
    Symbol copy = *sym;
    for (auto &f : copy.frames) for (auto &o : f) reassignIds(o);   // nested ids too
    return addSymbol(copy);
}

int Document::instanceCount(quint64 symId) const {
    int n = 0;
    auto count = [&](const QVector<VObject> &objs) {
        for (const VObject &o : objs)
            o.forEachNested([&](const VObject &x) { n += x.type == ObjType::Instance && x.symbolId == symId; });
    };
    for (const auto &f : s.frames) for (const auto &l : f.layers) count(l);
    // Instances that live only inside other symbols' artwork (same walk as deleteSymbol).
    for (const Symbol &sym : s.symbols) for (const auto &fr : sym.frames) count(fr);
    return n;
}

QVector<quint64> Document::breakApart() {
    QVector<quint64> out;
    auto &f = s.frames[curFrame];
    auto explode = [&](auto &&self, QVector<VObject> &box, int li) -> void {
        if (s.layers[li].locked) return;
        for (int i = box.size() - 1; i >= 0; --i) {
            if (box[i].type == ObjType::Group)
                self(self, box[i].children, li);
            if (i >= box.size()) continue;
            const VObject inst = box[i];
            if (inst.type != ObjType::Instance || !selection.contains(inst.id) || inst.locked) continue;
            const Symbol *sym = symbol(inst.symbolId);
            box.remove(i);
            if (!sym || sym->frames.isEmpty()) continue;
            const QTransform t = instanceTransform(inst, *sym);
            QVector<VObject> parts = sym->frames[qBound(0, inst.symFrame, int(sym->frames.size()) - 1)];
            for (VObject &c : parts) {
                reassignIds(c);
                placeObject(c, t.mapRect(c.bounds()));
                c.opacity = qRound(c.opacity * inst.opacity / 100.0);
                out << c.id;
            }
            for (int k = 0; k < parts.size(); ++k) box.insert(i + k, parts[k]);
        }
    };
    for (int li = 0; li < f.layers.size(); ++li)
        explode(explode, f.layers[li], li);
    selection = QSet<quint64>(out.begin(), out.end());
    emit selectionChanged();
    return out;
}

bool Document::tweenInstances(int steps, bool distribute) {
    steps = qBound(1, steps, 500);
    // selected instances of one symbol, including instances nested in groups, bottom to top
    struct Ref { int layer; quint64 parent; VObject o; };
    QVector<Ref> refs;
    const int startFrame = curFrame;
    auto walk = [&](auto &&self, QVector<VObject> &objs, int li, quint64 parent) -> void {
        for (VObject &o : objs) {
            if (o.type == ObjType::Instance && selection.contains(o.id) && !o.locked && !s.layers[li].locked)
                refs.append({li, parent, o});
            if (o.type == ObjType::Group) self(self, o.children, li, o.id);
        }
    };
    for (int li = 0; li < s.frames[startFrame].layers.size(); ++li)
        walk(walk, s.frames[startFrame].layers[li], li, 0);
    if (refs.size() < 2) return false;
    for (const Ref &r : refs) if (r.o.symbolId != refs.first().o.symbolId) return false;
    struct Place { int layer; quint64 parent; VObject o; };
    QVector<Place> seq;
    for (int k = 0; k + 1 < refs.size(); ++k) {
        const VObject &a = refs[k].o, &b = refs[k + 1].o;
        seq.append({refs[k].layer, refs[k].parent, refs[k].o});
        for (int i = 1; i <= steps; ++i) {
            const double t = double(i) / (steps + 1);
            VObject m = a;
            reassignIds(m);
            m.rect = lerpRect(a.rect.normalized(), b.rect.normalized(), t);
            m.opacity = qRound(a.opacity + (b.opacity - a.opacity) * t);
            seq.append({refs[k].layer, refs[k].parent, m});
        }
    }
    seq.append({refs.last().layer, refs.last().parent, refs.last().o});
    QSet<quint64> orig;
    for (const Ref &r : refs) orig.insert(r.o.id);
    auto eraseOrig = [&](auto &&self, QVector<VObject> &objs) -> void {
        objs.erase(std::remove_if(objs.begin(), objs.end(), [&](const VObject &o) { return orig.contains(o.id); }), objs.end());
        for (VObject &o : objs) if (o.type == ObjType::Group) self(self, o.children);
    };
    for (auto &l : s.frames[startFrame].layers) eraseOrig(eraseOrig, l);

    auto container = [&](int fi, int layer, quint64 parent) -> QVector<VObject> * {
        if (fi < 0 || fi >= s.frames.size()) return nullptr;
        if (layer < 0 || layer >= s.frames[fi].layers.size()) return nullptr;
        if (!parent) return &s.frames[fi].layers[layer];
        if (VObject *g = find(parent)) return &g->children;
        return &s.frames[fi].layers[layer];
    };

    QSet<quint64> sel;
    for (int k = 0; k < seq.size(); ++k) {
        const bool top = seq[k].parent == 0;
        int fi = (distribute && top) ? startFrame + k : startFrame;
        while (fi >= s.frames.size()) appendFrame();
        if (QVector<VObject> *box = container(fi, seq[k].layer, seq[k].parent))
            box->append(seq[k].o);
        if (fi == startFrame) sel.insert(seq[k].o.id);
    }
    selection = sel;
    emit selectionChanged();
    return true;
}

quint64 Document::animateSelection(int frames, const QPointF &move, double scalePct, int opacityFrom, int opacityTo) {
    frames = qBound(2, frames, 250);
    QVector<VObject> objs;
    for (VObject *o : selectedEditable()) if (!o->isWeb()) objs << *o;
    if (objs.isEmpty()) return 0;
    QRectF b;
    for (const VObject &o : objs) b = b.isNull() ? o.bounds() : b.united(o.bounds());
    QVector<QVector<VObject>> sf;
    for (int k = 0; k < frames; ++k) {
        const double t = double(k) / (frames - 1);
        const double sc = 1.0 + (scalePct / 100.0 - 1.0) * t;
        const QPointF c = b.center() + move * t;
        QVector<VObject> fr;
        for (VObject o : objs) {
            const QRectF ob = o.bounds();
            QRectF nb((ob.left() - b.center().x()) * sc + c.x(), (ob.top() - b.center().y()) * sc + c.y(), ob.width() * sc, ob.height() * sc);
            placeObject(o, nb);
            o.opacity = qBound(0, qRound(o.opacity * (opacityFrom + (opacityTo - opacityFrom) * t) / 100.0), 100);
            reassignIds(o);
            fr << o;
        }
        sf << fr;
    }
    const quint64 symId = convertToSymbol(QStringLiteral("Animation"), SymbolKind::Animation);
    if (!symId) return 0;
    // the instance created by convertToSymbol becomes frame 0; replace it with a spanning one
    const quint64 instId = *selection.begin();
    int layer = -1, index = -1;
    find(instId, &layer, &index);
    setSymbolFrames(symId, sf);
    VObject inst = makeInstance(symId);   // identity placement keeps the motion where it was drawn
    for (int k = 0; k < frames; ++k) {
        const int fi = curFrame + k;
        while (fi >= s.frames.size()) appendFrame();
        VObject o = inst;
        if (k == 0) o.id = instId; else reassignIds(o);
        o.symFrame = k;
        if (k == 0) s.frames[fi].layers[layer][index] = o;
        else s.frames[fi].layers[layer].append(o);
    }
    selection = {instId};
    emit selectionChanged();
    return symId;
}
