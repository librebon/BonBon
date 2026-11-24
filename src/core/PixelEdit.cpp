#include "PixelEdit.h"
#include "Effects.h"
#include "Filters.h"
#include <QTransform>
#include <cmath>

namespace pxedit {

QTransform docToImage(const VObject &o) {
    const double sx = o.rect.width() > 0 ? o.image.width() / o.rect.width() : 1;
    const double sy = o.rect.height() > 0 ? o.image.height() / o.rect.height() : 1;
    return QTransform::fromTranslate(-o.rect.x(), -o.rect.y()) * QTransform::fromScale(sx, sy);
}

QPointF docToImage(const VObject &o, const QPointF &d) {
    const double sx = o.rect.width() > 0 ? o.image.width() / o.rect.width() : 1;
    const double sy = o.rect.height() > 0 ? o.image.height() / o.rect.height() : 1;
    return QPointF((d.x() - o.rect.left()) * sx, (d.y() - o.rect.top()) * sy);
}

QTransform imageToDoc(const VObject &o) {
    const double sx = o.rect.width() > 0 ? o.image.width() / o.rect.width() : 1;
    const double sy = o.rect.height() > 0 ? o.image.height() / o.rect.height() : 1;
    // inverse of translate(-rect) * scale(sx,sy): scale(1/sx,1/sy) * translate(rect)
    return QTransform::fromScale(sx ? 1.0 / sx : 1.0, sy ? 1.0 / sy : 1.0)
         * QTransform::fromTranslate(o.rect.x(), o.rect.y());
}

QPoint imageShift(const VObject &o, const LiveEffect &e) {
    // the effect shifts the rendered object by whole canvas pixels (Render/Effects round with lround)
    const double sx = o.rect.width() > 0 ? o.image.width() / o.rect.width() : 1;
    const double sy = o.rect.height() > 0 ? o.image.height() / o.rect.height() : 1;
    return QPoint(int(std::lround(double(std::lround(e.offsetX)) * sx)), int(std::lround(double(std::lround(e.offsetY)) * sy)));
}

QString blockingEffect(const VObject &o) {
    for (const LiveEffect &e : o.effects)
        if (e.enabled && (e.type == EffectType::LensCorrection || e.type == EffectType::MakeSeamless)) return effectName(e.type);
    return {};
}

QString blockedMessage(const VObject &o, const QString &what) {
    return QStringLiteral("%1: the bitmap's %2 live effect moves its pixels, so edits would not land where you point. "
                          "Disable the effect (Property Inspector > Effects) or flatten the bitmap first.")
        .arg(what, blockingEffect(o));
}

bool enter(VObject &o, Session &s) {
    s = Session();
    if (o.type != ObjType::Bitmap || o.image.isNull() || o.image.width() <= 0 || o.image.height() <= 0) return false;
    for (int i = 0; i < o.effects.size(); ++i) {
        const LiveEffect &e = o.effects[i];
        if (!e.enabled || e.type != EffectType::Offset) continue;
        const QPoint sh = imageShift(o, e);
        if (sh.isNull()) continue;
        s.effectIndex << i;
        s.offsets << QPointF(e.offsetX, e.offsetY);
        s.shifts << sh;
        s.modes << e.undefinedAreas;
    }
    if (s.effectIndex.isEmpty()) return false;
    s.id = o.id;
    s.orig = o.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    s.rect = o.rect;
    QImage disp = s.orig;
    for (int k = 0; k < s.shifts.size(); ++k) disp = fx::offset(disp, s.shifts[k].x(), s.shifts[k].y(), s.modes[k]);
    for (int i : s.effectIndex) { o.effects[i].offsetX = 0; o.effects[i].offsetY = 0; }
    o.image = disp;
    s.displaySize = disp.size();
    return true;
}

void leave(VObject &o, const Session &s, bool *baked) {
    if (baked) *baked = false;
    if (s.effectIndex.isEmpty()) return;
    auto restoreOffsets = [&] {
        for (int k = 0; k < s.effectIndex.size(); ++k) {
            const int i = s.effectIndex[k];
            if (i < o.effects.size() && o.effects[i].type == EffectType::Offset) {
                o.effects[i].offsetX = s.offsets[k].x();
                o.effects[i].offsetY = s.offsets[k].y();
            }
        }
    };
    if (o.image.size() != s.displaySize || o.rect != s.rect) {
        // the bitmap was resized or moved: the old wrap box no longer exists, keep what is shown
        if (baked) *baked = true;
        return;
    }
    const int w = s.orig.width(), h = s.orig.height();
    // where each original column / row is shown (-1: hidden). Offsets are separable per axis.
    auto axisMap = [&](int n, bool xAxis) {
        QVector<int> m(n);
        for (int q = 0; q < n; ++q) {
            int p = q;
            for (int k = 0; k < s.shifts.size() && p >= 0; ++k) {
                p += xAxis ? s.shifts[k].x() : s.shifts[k].y();
                if (s.modes[k] == LiveEffect::WrapAround) { p %= n; if (p < 0) p += n; }
                else if (p < 0 || p >= n) p = -1;
            }
            m[q] = p;
        }
        return m;
    };
    const QVector<int> mx = axisMap(w, true), my = axisMap(h, false);
    const QImage edited = o.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const QRgb *org = reinterpret_cast<const QRgb *>(s.orig.constScanLine(y));
        if (my[y] < 0) { std::copy(org, org + w, d); continue; }
        const QRgb *e = reinterpret_cast<const QRgb *>(edited.constScanLine(my[y]));
        for (int x = 0; x < w; ++x) d[x] = mx[x] < 0 ? org[x] : e[mx[x]];
    }
    o.image = out;
    restoreOffsets();
}

bool isTransparent(const VObject &o) {
    return o.image.isNull() || filters::opaqueBounds(o.image).isEmpty();
}

bool trimToAlpha(VObject &o) {
    if (o.type != ObjType::Bitmap || o.image.isNull() || o.rect.isEmpty()) return false;
    if (!o.image.hasAlphaChannel()) return false;
    const QRect full = o.image.rect();
    const QRect keep = filters::opaqueBounds(o.image);
    if (keep == full) return false;
    const double sx = o.image.width() / o.rect.width(), sy = o.image.height() / o.rect.height();
    if (keep.isEmpty()) {
        if (o.image.size() == QSize(1, 1)) return false;
        QImage one(1, 1, QImage::Format_ARGB32_Premultiplied);
        one.fill(Qt::transparent);
        o.image = one;
        o.rect = QRectF(o.rect.left(), o.rect.top(), 1 / sx, 1 / sy);
        return true;
    }
    o.image = o.image.copy(keep);
    o.rect = QRectF(o.rect.left() + keep.left() / sx, o.rect.top() + keep.top() / sy, keep.width() / sx, keep.height() / sy);
    return true;
}

} // namespace pxedit
