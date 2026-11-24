// Groups and masks (v0.3.2), Fireworks MX 2004 style. See Document.h for the API.
#include "Document.h"
#include "Render.h"
#include <QPainter>

namespace {
void removeIdsKeep(QVector<VObject> &v, const QSet<quint64> &ids, quint64 keep) {
    for (int i = v.size() - 1; i >= 0; --i) {
        VObject &o = v[i];
        if (o.id != keep && ids.contains(o.id)) { v.removeAt(i); continue; }
        if (o.type == ObjType::Group && o.id != keep) {
            const bool had = !o.children.isEmpty();
            removeIdsKeep(o.children, ids, keep);
            removeIdsKeep(o.maskObj, ids, keep);
            if (had && o.children.isEmpty()) v.removeAt(i);
        }
    }
}

// a bitmap with transparent pixels masks by its alpha; an opaque one by its gray levels
int defaultMaskMode(const VObject &m) {
    if (m.type == ObjType::Bitmap) {
        if (!m.image.hasAlphaChannel()) return VObject::MaskGrayscale;
        const QImage img = m.image.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y) {
            const QRgb *l = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); ++x) if (qAlpha(l[x]) < 255) return VObject::MaskOutline;
        }
        return VObject::MaskGrayscale;
    }
    return VObject::MaskOutline;
}
}

quint64 Document::groupSelection() {
    QVector<VObject *> sel = selectedOutermost();
    QVector<VObject> members;
    QSet<quint64> ids;
    quint64 topId = 0;
    for (VObject *o : std::as_const(sel)) {
        if (o->isWeb() || isMaskObject(o->id)) continue;
        members << *o;
        ids << o->id;
        topId = o->id;
    }
    if (members.isEmpty()) return 0;
    VObject g;
    g.type = ObjType::Group;
    g.id = newId();
    g.children = members;
    int layer = 0, index = 0;
    QVector<VObject> *c = containerOf(topId, &layer, &index);
    if (!c) return 0;
    (*c)[index] = g;              // the group takes the top member's place in the stacking order
    for (auto &l : frame().layers) removeIdsKeep(l, ids, g.id);
    curLayer = layer;
    selectOnly(g.id);
    return g.id;
}

QVector<quint64> Document::ungroupSelection() {
    QVector<quint64> groups, out;
    for (VObject *o : selectedOutermost()) if (o->type == ObjType::Group) groups << o->id;
    if (groups.isEmpty()) return out;
    QSet<quint64> newSel;
    for (VObject *o : selectedObjects()) newSel << o->id;
    for (quint64 gid : std::as_const(groups)) {
        int layer = 0, index = 0;
        QVector<VObject> *c = containerOf(gid, &layer, &index);
        if (!c) continue;
        const VObject g = (*c)[index];
        QVector<VObject> rel = g.children;
        for (VObject &m : rel) {
            // the group's opacity and blend carry over to its members
            if (g.opacity < 100) m.opacity = qRound(m.opacity * g.opacity / 100.0);
            if (g.blend != BlendMode::Normal && m.blend == BlendMode::Normal) m.blend = g.blend;
        }
        for (VObject m : g.maskObj) { m.visible = true; rel << m; } // a released mask becomes an object on top
        c->removeAt(index);
        for (int i = 0; i < rel.size(); ++i) { c->insert(index + i, rel[i]); newSel << rel[i].id; out << rel[i].id; }
        newSel.remove(gid);
        if (enteredGroup == gid) enteredGroup = 0;
    }
    setSelection(newSel);
    return out;
}

// The group that a mask command works on: a single selected group, or a new group of the selection.
static quint64 maskTarget(Document &d) {
    QVector<VObject *> sel = d.selectedOutermost();
    if (sel.size() == 1 && sel[0]->type == ObjType::Group) return sel[0]->id;
    return d.groupSelection();
}

quint64 Document::pasteAsMask(const VObject &mask) {
    const quint64 gid = maskTarget(*this);
    VObject *g = gid ? find(gid) : nullptr;
    if (!g) return 0;
    VObject m = mask;
    reassignIds(m);
    m.visible = true;
    g->maskObj = {m};
    g->maskMode = defaultMaskMode(m);
    g->maskEnabled = true;
    g->maskLinked = true;
    selectOnly(gid);
    return gid;
}

quint64 Document::groupAsMask() {
    QVector<VObject *> sel = selectedOutermost();
    QVector<VObject *> usable;
    for (VObject *o : sel) if (!o->isWeb() && !isMaskObject(o->id)) usable << o;
    if (usable.size() < 2) return 0;
    const VObject mask = *usable.last();
    QSet<quint64> rest;
    for (int i = 0; i + 1 < usable.size(); ++i) rest << usable[i]->id;
    for (auto &l : frame().layers) removeIdsKeep(l, {mask.id}, 0);
    selection = rest;
    const quint64 gid = groupSelection();
    VObject *g = gid ? find(gid) : nullptr;
    if (!g) return 0;
    VObject m = mask;
    m.visible = true;
    g->maskObj = {m};
    g->maskMode = defaultMaskMode(m);
    g->maskEnabled = g->maskLinked = true;
    return gid;
}

quint64 Document::pasteInside(const QVector<VObject> &content) {
    QVector<VObject *> sel = selectedOutermost();
    if (sel.size() != 1 || content.isEmpty() || sel[0]->isWeb()) return 0;
    int layer = 0, index = 0;
    QVector<VObject> *c = containerOf(sel[0]->id, &layer, &index);
    if (!c) return 0;
    VObject m = (*c)[index];
    m.visible = true;
    VObject g;
    g.type = ObjType::Group;
    g.id = newId();
    for (VObject o : content) { reassignIds(o); g.children << o; }
    g.maskObj = {m};
    g.maskMode = defaultMaskMode(m);
    (*c)[index] = g;
    curLayer = layer;
    selectOnly(g.id);
    return g.id;
}

quint64 Document::revealHideAll(bool reveal) {
    const quint64 gid = maskTarget(*this);
    VObject *g = gid ? find(gid) : nullptr;
    if (!g) return 0;
    QRectF vb;
    for (const VObject &c : g->children) vb = vb.isNull() ? c.visualBounds() : vb.united(c.visualBounds());
    QRect area = vb.toAlignedRect();
    if (area.isEmpty()) area = QRect(0, 0, 1, 1);
    VObject m;
    m.type = ObjType::Bitmap;
    m.stampInterpolation();
    m.id = newId();
    m.name = QStringLiteral("Mask");
    m.image = QImage(area.size(), QImage::Format_ARGB32_Premultiplied);
    m.image.fill(reveal ? Qt::white : Qt::black);
    m.rect = QRectF(area);
    g->maskObj = {m};
    g->maskMode = VObject::MaskGrayscale;
    g->maskEnabled = g->maskLinked = true;
    selectOnly(gid);
    return gid;
}

static QVector<quint64> maskOwners(Document &d) {
    QVector<quint64> out;
    for (VObject *o : d.selectedObjects()) {
        VObject *g = o->hasMask() ? o : nullptr;
        for (VObject *p = g ? nullptr : d.parentGroup(o->id); p && !g; p = d.parentGroup(p->id)) if (p->hasMask()) g = p;
        if (g && !out.contains(g->id)) out << g->id;
    }
    return out;
}

quint64 Document::maskOwnerOfSelection() {
    const QVector<quint64> o = maskOwners(*this);
    return o.size() == 1 ? o[0] : 0;
}

int Document::setMaskEnabled(bool on) {
    int n = 0;
    for (quint64 id : maskOwners(*this)) if (VObject *g = find(id); g && g->maskEnabled != on) { g->maskEnabled = on; ++n; }
    return n;
}

int Document::setMaskLinked(bool on) {
    int n = 0;
    for (quint64 id : maskOwners(*this)) if (VObject *g = find(id); g && g->maskLinked != on) { g->maskLinked = on; ++n; }
    return n;
}

int Document::deleteMask() {
    int n = 0;
    QSet<quint64> sel;
    for (quint64 id : maskOwners(*this)) if (VObject *g = find(id)) { g->maskObj.clear(); ++n; sel << id; }
    if (n) setSelection(sel);
    return n;
}

QVector<quint64> Document::convertToBitmap() {
    QVector<quint64> ids, out;
    for (VObject *o : selectedOutermost()) if (!o->isWeb() && !isMaskObject(o->id)) ids << o->id;
    SymbolScope scope(&s.symbols);
    const QRect big(-1000000, -1000000, 2000000, 2000000);
    for (quint64 id : std::as_const(ids)) {
        int layer = 0, index = 0;
        QVector<VObject> *c = containerOf(id, &layer, &index);
        if (!c) continue;
        const VObject src = (*c)[index];
        VObject plain = src;
        plain.visible = true;
        plain.opacity = 100;
        plain.blend = BlendMode::Normal;
        QRect area;
        QImage img = render::renderObject(plain, &area, big);
        VObject b;
        b.type = ObjType::Bitmap;
        b.id = newId();
        b.name = src.name;
        b.visible = src.visible;
        b.locked = src.locked;
        b.opacity = src.opacity;
        b.blend = src.blend;
        if (img.isNull() || area.isEmpty()) { area = QRect(src.bounds().toAlignedRect().topLeft(), QSize(1, 1)); img = QImage(1, 1, QImage::Format_ARGB32_Premultiplied); img.fill(0); }
        b.image = img;
        b.rect = QRectF(area);
        b.stampInterpolation();
        (*c)[index] = b;
        out << b.id;
    }
    if (!out.isEmpty()) setSelection(QSet<quint64>(out.begin(), out.end()));
    return out;
}
