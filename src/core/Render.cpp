#include "Render.h"
#include "AutoShape.h"
#include "Document.h"
#include "Effects.h"
#include "Fills.h"
#include "CorePrefs.h"
#include "PixelScale.h"
#include "io/LwdFormat.h"
#include <QCache>
#include <QCryptographicHash>
#include <QFontMetricsF>
#include <QMutex>
#include <atomic>
#include <cmath>
#include <QSet>
#include <QDebug>

namespace render {

namespace {
// A scaled bitmap (its rectangle or transform differs from its pixel size) is drawn from a copy
// resampled to its exact device size with the Preferences interpolation: the edge-clamped
// premultiplied Bicubic / Bilinear / Soft filters (v0.3.4) or a pixel-art scaler (next whole factor
// up, then resampled down). Bitmap scaling stays non-destructive, so the copies are cached by image,
// size and method. Canvas zoom is not affected: the canvas shows the document-size render.
QImage displayScaled(const QImage &img, const QSize &size, int method) {
    static QMutex mutex;
    static QCache<QString, QImage> cache(256 * 1024 * 1024 / 4);   // cost = pixels (about 256 MB)
    if (method < 0) method = coreprefs::interpolation;
    const QString key = QStringLiteral("%1/%2x%3/%4").arg(img.cacheKey()).arg(size.width()).arg(size.height()).arg(method);
    {
        QMutexLocker lock(&mutex);
        if (QImage *hit = cache.object(key)) return *hit;
    }
    QImage out = pixelscale::resample(img, size, method);
    QMutexLocker lock(&mutex);
    cache.insert(key, new QImage(out), int(qMin<qint64>(qint64(size.width()) * size.height(), INT_MAX)));
    return out;
}
}

namespace {
// Per-object visual cache (0.3.15+): renderObject output keyed by content digest only (0.3.16
// dropped clip from the key to avoid duplicates; blit uses the painter clip). Instance keys
// include a symbol body digest that nests into referenced symbols (0.3.17). Invalidates
// itself when any drawable field or bitmap cacheKey changes (edits, effects, transforms, text,
// fills, undo/redo restoring different pixels, etc.).
struct VisualEntry {
    QImage img;
    QRect area;
};
QMutex g_visualMu;
QCache<QByteArray, VisualEntry> g_visualCache(512 * 1024); // cost = KB of image data
std::atomic<bool> g_visualEnabled{true};

// Body blit cache: a symbol rasterised at a given linear/device scale, reused by every instance
// that shares that scale. Pixel-identical to drawing the children under the same transform.
struct BodyBlitEntry {
    QImage img;
    QRect area; // device pixels
};
QMutex g_bodyMu;
QCache<QByteArray, BodyBlitEntry> g_bodyCache(256 * 1024);
std::atomic<bool> g_bodyEnabled{true};

QByteArray symbolBodyKey(quint64 symbolId, int depth, QSet<quint64> &stack) {
    const Symbol *sym = findSymbol(symbolId);
    if (!sym) return QByteArrayLiteral("sym:missing");
    if (stack.contains(symbolId))
        return QByteArrayLiteral("sym:cycle:") + QByteArray::number(symbolId);
    // A cached digest is only published from a complete depth-0 walk.
    if (!sym->bodyKeyCache.isEmpty()) return sym->bodyKeyCache;
    if (depth >= 8) return QByteArrayLiteral("sym:depth:") + QByteArray::number(symbolId);
    stack.insert(symbolId);
    QByteArray out;
    out += QByteArray::number(quint64(sym->id));
    out += ':';
    out += QByteArray::number(int(sym->kind));
    for (const auto &fr : sym->frames) {
        out += '|';
        for (const VObject &c : fr) {
            out += lwd::objectContentKey(c);
            // Nested instances, including those inside Groups / masks: fold the referenced
            // symbol's body so Edit Symbol on an inner symbol invalidates outer FX keys.
            c.forEachNested([&](const VObject &x) {
                if (x.type == ObjType::Instance) {
                    out += '@';
                    out += symbolBodyKey(x.symbolId, depth + 1, stack);
                }
            });
            out += ';';
        }
    }
    stack.remove(symbolId);
    const QByteArray digest = QCryptographicHash::hash(out, QCryptographicHash::Sha1);
    if (depth == 0) sym->bodyKeyCache = digest;
    return digest;
}
QByteArray symbolBodyKey(quint64 symbolId) {
    QSet<quint64> stack;
    return symbolBodyKey(symbolId, 0, stack);
}

// Content-only key (no clip): cache the full object visual and let the painter clip on blit.
QByteArray visualKey(const VObject &o) {
    QByteArray k = lwd::objectContentKey(o);
    if (o.type == ObjType::Instance) {
        k.append("|sym|");
        k.append(symbolBodyKey(o.symbolId));
    }
    return k;
}
}

QByteArray symbolBodyDigest(quint64 symbolId) { return symbolBodyKey(symbolId); }

void clearSymbolBodyBlitCache() {
    QMutexLocker lock(&g_bodyMu);
    g_bodyCache.clear();
}
void setSymbolBodyBlitCacheEnabled(bool on) {
    g_bodyEnabled.store(on, std::memory_order_relaxed);
    if (!on) clearSymbolBodyBlitCache();
}
bool symbolBodyBlitCacheEnabled() { return g_bodyEnabled.load(std::memory_order_relaxed); }
int symbolBodyBlitCacheEntries() {
    QMutexLocker lock(&g_bodyMu);
    return g_bodyCache.count();
}

void clearObjectVisualCache() {
    QMutexLocker lock(&g_visualMu);
    g_visualCache.clear();
    clearSymbolBodyBlitCache();
}
void setObjectVisualCacheEnabled(bool on) {
    g_visualEnabled.store(on, std::memory_order_relaxed);
    if (!on) clearObjectVisualCache();
}
bool objectVisualCacheEnabled() { return g_visualEnabled.load(std::memory_order_relaxed); }
int objectVisualCacheEntries() {
    QMutexLocker lock(&g_visualMu);
    return g_visualCache.count();
}

namespace {
// A bitmap drawn axis-aligned covers whole device pixels (v0.3.4). Before, a scaled bitmap whose edge
// fell between pixels (the Scale tool's left / top handles, Image Size, numeric scaling) had that
// edge drawn with partial coverage by the smooth (Bicubic / Bilinear / Soft) path, leaving a light
// 1 px strip of background showing through; Nearest Neighbour already snapped. Rotated or skewed
// bitmaps keep their antialiased edges.
QRectF snappedDeviceRect(const QTransform &t, const QRectF &r) {
    const QRectF d = t.mapRect(r);
    const double l = std::round(d.left()), tp = std::round(d.top());
    const double rt = qMax(l + 1, std::round(d.right())), b = qMax(tp + 1, std::round(d.bottom()));
    return QRectF(QPointF(l, tp), QPointF(rt, b));
}

void drawBitmapImage(QPainter &p, const QRectF &r, const QImage &img, int method) {
    const QTransform t = p.transform();
    if (t.type() > QTransform::TxScale || r.isEmpty()) {   // rotated / skewed / perspective
        p.drawImage(r, img);
        return;
    }
    const QRectF dr = snappedDeviceRect(t, r);
    const QSize target(int(dr.width()), int(dr.height()));
    const bool flipX = t.m11() < 0, flipY = t.m22() < 0;
    QImage shown;
    if (target == img.size())
        shown = img;
    else if (qint64(target.width()) * target.height() <= 64LL * 1024 * 1024)
        shown = displayScaled(img, target, method);   // per-object interpolation (0.3.16)
    if (shown.isNull()) {                     // a huge target: Qt's own path into the snapped rectangle
        bool ok = false;
        const QTransform inv = t.inverted(&ok);
        p.drawImage(ok ? inv.mapRect(dr) : r, img);
        return;
    }
    if (flipX || flipY) shown = shown.mirrored(flipX, flipY);
    p.save();
    p.resetTransform();
    p.drawImage(dr.topLeft(), shown);
    p.restore();
}
}

namespace {
const QRect kBig(-1000000, -1000000, 2000000, 2000000);

// The mask's coverage, drawn through `t` into an image of `size` (ARGB32 premultiplied; the
// coverage is the alpha for outline / alpha masks and gray x alpha for grayscale masks).
QImage maskCoverage(const VObject &group, const QTransform &t, const QSize &size) {
    const VObject &m = group.maskObj[0];
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    {
        QPainter p(&img);
        p.setTransform(t);
        VObject c = m;
        c.visible = true;
        c.quickMask = false;
        const bool vectorOutline = group.maskMode == VObject::MaskOutline && m.isVector() && m.type != ObjType::Line;
        if (vectorOutline) {
            // path outline: the shape's area, whatever its fill and stroke look like
            QPainterPath path = m.type == ObjType::Text ? (m.hasXform() ? m.xform.map(m.untransformed().textPath()) : m.textPath()) : m.shapePath();
            path.setFillRule(Qt::WindingFill);
            p.setRenderHint(QPainter::Antialiasing, m.antialias);
            p.fillPath(path, Qt::white);
        } else {
            drawObject(p, c, kBig);
        }
    }
    if (group.maskMode == VObject::MaskGrayscale) {
        for (int y = 0; y < img.height(); ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = d[x];
                const int a = qAlpha(px);
                if (!a) continue;
                // premultiplied: gray of the colour channels already carries the alpha
                const int g = (qRed(px) * 11 + qGreen(px) * 16 + qBlue(px) * 5) / 32;
                d[x] = qRgba(0, 0, 0, qMin(a, g));
            }
        }
    }
    return img;
}

void multiplyByCoverage(QImage &content, const QImage &cov) {
    for (int y = 0; y < content.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(content.scanLine(y));
        const QRgb *m = reinterpret_cast<const QRgb *>(cov.constScanLine(y));
        for (int x = 0; x < content.width(); ++x) {
            const int a = qAlpha(m[x]);
            if (a == 255) continue;
            if (a == 0) { d[x] = 0; continue; }
            const QRgb px = d[x];
            auto mul = [a](int v) { return (v * a + 127) / 255; };
            d[x] = qRgba(mul(qRed(px)), mul(qGreen(px)), mul(qBlue(px)), mul(qAlpha(px)));
        }
    }
}

void drawGroup(QPainter &p, const VObject &o) {
    static thread_local int depth = 0;
    if (depth > 32) return;
    ++depth;
    if (!o.maskActive()) {
        for (const VObject &c : o.children) drawObject(p, c, kBig);
        --depth;
        return;
    }
    // masked: render the members and the mask at device resolution, multiply, draw
    const QTransform t = p.transform();
    QRect dev = t.mapRect(o.visualBounds()).toAlignedRect().adjusted(-1, -1, 1, 1);
    if (p.device()) dev = dev.intersected(QRect(0, 0, p.device()->width(), p.device()->height()));
    if (p.hasClipping()) dev = dev.intersected(t.mapRect(p.clipBoundingRect()).toAlignedRect().adjusted(-1, -1, 1, 1));
    if (dev.isEmpty()) { --depth; return; }
    const QTransform local = t * QTransform::fromTranslate(-dev.left(), -dev.top());
    QImage content(dev.size(), QImage::Format_ARGB32_Premultiplied);
    content.fill(0);
    {
        QPainter cp(&content);
        cp.setTransform(local);
        for (const VObject &c : o.children) drawObject(cp, c, kBig);
    }
    multiplyByCoverage(content, maskCoverage(o, local, dev.size()));
    --depth;
    p.save();
    p.resetTransform();
    p.drawImage(dev.topLeft(), content);
    p.restore();
}
}

void drawBase(QPainter &p, const VObject &o) {
    if (o.type == ObjType::Group) { drawGroup(p, o); return; }
    if (o.hasXform()) {
        // non-destructive transform: draw the untouched object through it
        p.save();
        p.setTransform(o.xform, true);
        drawBase(p, o.untransformed());
        p.restore();
        return;
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing, o.antialias);
    p.setRenderHint(QPainter::TextAntialiasing, o.antialias);
    // rotated / skewed / distorted bitmaps always resample smoothly; axis-aligned scaling uses the
    // per-object stamped interpolation (Preferences only for unset / legacy objects).
    const bool smoothBmp = (o.type == ObjType::Bitmap ? o.effectiveInterpolation() : coreprefs::interpolation) < coreprefs::Nearest;
    p.setRenderHint(QPainter::SmoothPixmapTransform, smoothBmp || p.transform().type() > QTransform::TxScale);
    switch (o.type) {
    case ObjType::Bitmap:
        if (!o.image.isNull() && o.quickMask) {
            // Quick Mask: white = selected (clear), black / transparent = masked (50 % red)
            const QImage g = o.image.convertToFormat(QImage::Format_ARGB32);
            QImage ov(g.size(), QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < g.height(); ++y) {
                const QRgb *s = reinterpret_cast<const QRgb *>(g.constScanLine(y));
                QRgb *d = reinterpret_cast<QRgb *>(ov.scanLine(y));
                for (int x = 0; x < g.width(); ++x) {
                    const int v = (qGray(s[x]) * qAlpha(s[x]) + 127) / 255;
                    d[x] = qPremultiply(qRgba(255, 0, 0, (255 - v) / 2));
                }
            }
            p.drawImage(o.rect.normalized(), ov);
        } else if (!o.image.isNull()) {
            drawBitmapImage(p, o.rect.normalized(), o.image, o.effectiveInterpolation());
        }
        break;
    case ObjType::Text: {
        // Every text AA mode is honoured both by glyph rasterisation (font style strategy)
        // and by path filling (QPainter::Antialiasing), so canvas and export agree.
        const bool aa = o.textAA != TextAA::None;
        p.setRenderHint(QPainter::Antialiasing, aa);
        p.setRenderHint(QPainter::TextAntialiasing, aa);
        const QFont f = o.font();
        const bool outline = o.strokeOn && o.strokeWidth > 0;
        const bool embolden = o.textAA == TextAA::Strong;
        const bool fancyFill = o.fillKind != FillKind::Solid || o.feather > 0 || (!o.texture.isEmpty() && o.textureAmount > 0);
        if (outline || embolden || fancyFill || o.textOnPath) {
            QPainterPath tp = o.textPath();
            if (o.fillOn) {
                fills::fillShape(p, o, tp);
                if (embolden && o.fillKind == FillKind::Solid) // Strong: thicken glyphs slightly, like Fireworks' heavier AA
                    p.strokePath(tp, QPen(o.fill, qBound(0.35, o.fontSize / 40.0, 1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            }
            if (outline) fills::strokeShape(p, o, tp, true);
        } else if (o.fillOn) {
            p.setFont(f);
            p.setPen(o.fill);
            for (const auto &l : o.textLines()) p.drawText(l.baseline, l.text);
        }
        break;
    }
    case ObjType::Instance: {
        static thread_local int depth = 0;
        const Symbol *sym = findSymbol(o.symbolId);
        if (!sym || sym->frames.isEmpty() || depth > 8) break;
        const auto &objs = sym->frames[qBound(0, o.symFrame, int(sym->frames.size()) - 1)];
        const QTransform inst = instanceTransform(o, *sym);
        const QTransform T = inst * p.transform();
        const QRectF sb = sym->visualBounds();
        QRect area = T.mapRect(sb).toAlignedRect().adjusted(-2, -2, 2, 2);
        const bool bodyOn = g_bodyEnabled.load(std::memory_order_relaxed);
        const bool areaOk = !area.isEmpty() && area.width() <= 8192 && area.height() <= 8192
                            && qint64(area.width()) * area.height() <= 16LL * 1024 * 1024;
        if (bodyOn && areaOk) {
            auto qk = [](double v) { return std::llround(v * 1024.0); };
            auto qf = [](double v) { return int(std::llround((v - std::floor(v)) * 64.0)); };
            QByteArray key = symbolBodyKey(o.symbolId);
            key += '|';
            key += QByteArray::number(o.symFrame);
            key += '|';
            key += QByteArray::number(qk(T.m11())) + ',' + QByteArray::number(qk(T.m12())) + ','
                 + QByteArray::number(qk(T.m21())) + ',' + QByteArray::number(qk(T.m22())) + ','
                 + QByteArray::number(qf(T.dx())) + ',' + QByteArray::number(qf(T.dy()));
            key += '|';
            key += QByteArray::number(area.width()) + 'x' + QByteArray::number(area.height());
            key += '|';
            key += (p.renderHints() & QPainter::Antialiasing) ? 'A' : 'a';
            key += (p.renderHints() & QPainter::SmoothPixmapTransform) ? 'S' : 's';
            key += (p.renderHints() & QPainter::TextAntialiasing) ? 'T' : 't';
            QImage img;
            QRect cachedArea;
            {
                QMutexLocker lock(&g_bodyMu);
                if (BodyBlitEntry *hit = g_bodyCache.object(key)) {
                    img = hit->img;
                    cachedArea = hit->area;
                }
            }
            if (img.isNull()) {
                img = QImage(area.size(), QImage::Format_ARGB32_Premultiplied);
                img.fill(0);
                {
                    QPainter ip(&img);
                    ip.setRenderHints(p.renderHints());
                    // Same order as drawGroup: map through T, then shift so area.topLeft() is (0,0).
                    const QTransform local = T * QTransform::fromTranslate(-area.left(), -area.top());
                    ip.setWorldTransform(local);
                    ++depth;
                    const QRect big(-1000000, -1000000, 2000000, 2000000);
                    for (const VObject &c : objs) drawObject(ip, c, big);
                    --depth;
                }
                QMutexLocker lock(&g_bodyMu);
                g_bodyCache.insert(key, new BodyBlitEntry{img, area}, qMax(1, int(img.sizeInBytes() / 1024)));
                cachedArea = area;
            }
            // Blit in device space. The cached image covers `cachedArea`; for a hit the
            // integer translation may differ, so shift by the area origin we computed.
            const QPoint dest = area.topLeft();
            p.save();
            const QTransform saved = p.worldTransform();
            p.resetTransform();
            p.drawImage(dest, img);
            p.setWorldTransform(saved);
            p.restore();
            break;
        }
        ++depth;
        p.setTransform(inst, true);
        const QRect big(-1000000, -1000000, 2000000, 2000000);
        for (const VObject &c : objs) drawObject(p, c, big);
        --depth;
        break;
    }
    case ObjType::AutoShape: {
        const autoshape::Geometry g = autoshape::geometry(o);
        if (o.fillOn && !g.fill.isEmpty()) fills::fillShape(p, o, g.fill);
        if (!g.fill.isEmpty()) fills::strokeShape(p, o, g.fill, true);
        if (!g.open.isEmpty()) fills::strokeShape(p, o, g.open, false);
        if (o.strokeOn && !g.heads.isEmpty()) {
            QPainterPath hp = g.heads;
            hp.setFillRule(Qt::WindingFill);
            p.fillPath(hp, o.stroke);
        }
        break;
    }
    default: {
        QPainterPath path = o.shapePath();
        if (o.fillOn && o.type != ObjType::Line) fills::fillShape(p, o, path);
        const bool closedShape = o.type == ObjType::Rect || o.type == ObjType::Ellipse || (o.type == ObjType::Path && o.closed);
        fills::strokeShape(p, o, path, closedShape);
    }
    }
    p.restore();
}

QImage renderObject(const VObject &o, QRect *areaOut, const QRect &clip) {
    // Cache key is content-only (no clip). Always build the full visualBounds buffer so FX edges
    // match a full-frame render; the painter clips on blit. `clip` is used only to skip work when
    // the dirty/view rect cannot touch this object (0.3.17).
    QRect area = o.visualBounds().toAlignedRect();
    if (area.isEmpty()) { if (areaOut) *areaOut = QRect(); return {}; }
    if (!clip.isNull()) {
        bool wraps = false;
        for (const auto &e : o.effects)
            wraps |= e.enabled && (e.type == EffectType::Offset || e.type == EffectType::NormalMap || e.type == EffectType::AmbientOcclusion);
        if (!wraps) {
            const int m = int(std::ceil(qMax(o.effectMargin(), o.effectReach()) + 2));
            if (!area.intersects(clip.adjusted(-m, -m, m, m))) {
                if (areaOut) *areaOut = QRect();
                return {};
            }
        }
    }
    if (o.type == ObjType::Bitmap && o.interpolation < 0) {
#if !defined(NDEBUG)
        // After Open/addObject/resetHistory, bitmaps must be stamped; -1 here means a create path skipped the helper.
        Q_ASSERT_X(false, "renderObject",
                   "bitmap rendered with unset Scaling (-1); Open/addObject should have stamped it");
#endif
        // Release: draw with the live Preferences default (effectiveInterpolation) but do not
        // mutate the document — a const_cast stamp raced the save-thumbnail worker.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            qWarning("renderObject: bitmap had unset Scaling; drawing with the live Preferences default");
    }
    bool anyFxEarly = false;
    for (const auto &e : o.effects) anyFxEarly |= e.enabled;
    // Instances without effects are already in the body blit cache; don't store them twice.
    const bool useVisual = g_visualEnabled.load(std::memory_order_relaxed)
                           && !(o.type == ObjType::Instance && !anyFxEarly);
    QByteArray key;
    if (useVisual) {
        key = visualKey(o);
        QMutexLocker lock(&g_visualMu);
        if (VisualEntry *hit = g_visualCache.object(key)) {
            if (areaOut) *areaOut = hit->area;
            return hit->img;
        }
    }
    QImage img(area.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    {
        QPainter p(&img);
        p.translate(-area.topLeft());
        drawBase(p, o);
    }
    bool anyFx = false;
    for (const auto &e : o.effects) anyFx |= e.enabled;
    if (anyFx) {
        // the object's own box in buffer pixels (bitmaps: exactly their pixel grid)
        const QRectF b = o.bounds().translated(-area.topLeft());
        const QRect objRect(QPoint(qRound(b.left()), qRound(b.top())), QPoint(qRound(b.right()) - 1, qRound(b.bottom()) - 1));
        img = fx::applyEffects(img, o.effects, objRect);
    }
    if (areaOut) *areaOut = area;
    if (useVisual && !img.isNull()) {
        QMutexLocker lock(&g_visualMu);
        // another thread may have filled it; insert is fine either way
        g_visualCache.insert(key, new VisualEntry{img, area}, qMax(1, int(img.sizeInBytes() / 1024)));
    }
    return img;
}

void drawObject(QPainter &p, const VObject &o, const QRect &clip) {
    if (!o.visible || o.opacity <= 0 || o.isWeb()) return; // slices/hotspots never become pixels
    bool anyFx = false;
    for (const auto &e : o.effects) anyFx |= e.enabled;
    if (!anyFx && o.opacity >= 100 && o.blend == BlendMode::Normal) {
        drawBase(p, o);
        return;
    }
    QRect area;
    QImage img = renderObject(o, &area, clip);
    if (img.isNull()) return;
    drawBlended(p, img, QRect(area.topLeft(), img.size()), o.blend, o.opacity / 100.0);
}

QImage renderFrame(const DocState &st, int frame, bool withBackground) {
    QImage img(qMax(1, st.width), qMax(1, st.height), QImage::Format_ARGB32_Premultiplied);
    img.fill(withBackground ? st.canvasColor : QColor(Qt::transparent));
    if (frame < 0 || frame >= st.frames.size()) return img;
    QPainter p(&img);
    SymbolScope scope(&st.symbols);
    QRect clip(0, 0, st.width, st.height);
    const Frame &f = st.frames[frame];
    for (int li = 0; li < st.layers.size() && li < f.layers.size(); ++li) {
        if (!st.layers[li].visible || st.layers[li].web) continue;
        for (const VObject &o : f.layers[li]) drawObject(p, o, clip);
    }
    return img;
}

QImage renderObjects(const QVector<VObject> &objs, const QRect &area) {
    // composite in 16 bits per channel so stacking semi-transparent objects rounds once, not per object
    QImage img(area.size(), QImage::Format_RGBA64_Premultiplied);
    img.fill(0);
    {
        QPainter p(&img);
        p.translate(-area.topLeft());
        for (const VObject &o : objs) drawObject(p, o, area);
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}


} // namespace render
