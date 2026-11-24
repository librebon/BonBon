#include "Brand.h"
// Document-level import / export: layered raster files, SVG, PDF, editable PNG and favicon bundles.
#include "DocIO.h"
#include "core/PixelEdit.h"
#include "LwdFormat.h"
#include "core/Document.h"
#include "core/Render.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>

namespace docio {
using codec::setErr;

bool opensAsDocument(Fmt f) {
    return f == Fmt::PSD || f == Fmt::ORA || f == Fmt::XCF || f == Fmt::SVG || f == Fmt::SVGZ;
}

bool isEditablePng(const QByteArray &d) {
    return d.startsWith("\x89PNG\r\n\x1a\n") && !codec::pngChunk(d, "lwDC").isEmpty();
}

namespace {
static const char kCanvasLayer[] = "Canvas";

VObject bitmapFromLayer(Document *d, const codec::RasterLayer &rl, const QString &name) {
    VObject o;
    o.id = d->newId();
    o.type = ObjType::Bitmap;
    o.name = name;
    o.image = rl.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    o.rect = QRectF(rl.offset, rl.image.size());
    pxedit::trimToAlpha(o);
    o.opacity = qBound(0, qRound(rl.opacity * 100 / 255.0), 100);
    o.blend = BlendMode(qBound(0, rl.blend, int(BlendMode::Exclusion)));
    o.fillOn = o.strokeOn = false;
    o.stampInterpolation();
    return o;
}
// a file's layer group (layers `idx`, bottom first) -> a group object; nested groups recurse
VObject groupFromLayers(Document *d, const codec::LayeredImage &li, const QVector<int> &idx, int g, int depth) {
    const codec::LayerGroup &G = li.groups[g];
    VObject grp;
    grp.type = ObjType::Group;
    grp.name = G.name.isEmpty() ? QStringLiteral("Group") : G.name;
    grp.opacity = qBound(0, qRound(G.opacity * 100 / 255.0), 100);
    grp.blend = BlendMode(qBound(0, G.blend, int(BlendMode::Exclusion)));
    grp.visible = G.visible;
    grp.fillOn = grp.strokeOn = false;
    for (int k = 0; k < idx.size();) {
        const codec::RasterLayer &rl = li.layers[idx[k]];
        const int sub = rl.group == g ? -1 : codec::groupChildOf(li.groups, rl.group, g);
        if (sub < 0 || depth > 32) {
            ++k;
            if (rl.image.isNull()) continue;
            VObject o = bitmapFromLayer(d, rl, rl.name.isEmpty() ? QStringLiteral("Bitmap") : rl.name);
            o.visible = rl.visible;
            grp.children << o;
            continue;
        }
        QVector<int> run;
        while (k < idx.size() && li.layers[idx[k]].group != g && codec::groupChildOf(li.groups, li.layers[idx[k]].group, g) == sub) run << idx[k++];
        VObject c = groupFromLayers(d, li, run, sub, depth + 1);
        if (!c.children.isEmpty()) grp.children << c;
    }
    grp.id = d->newId();
    return grp;
}

Document *fromLayered(const codec::LayeredImage &li, QStringList *notes) {
    auto *d = new Document;
    d->s.width = qMax(1, li.width);
    d->s.height = qMax(1, li.height);
    d->s.canvasColor = Qt::transparent;
    d->s.layers.clear();
    d->s.frames.clear();
    Frame f;
    f.name = QStringLiteral("Frame 1");
    for (int k = 0; k < li.layers.size(); ++k) {
        const codec::RasterLayer &rl = li.layers[k];
        if (rl.group >= 0) {
            // a top-level layer group -> one document layer holding a group object (v0.3.2)
            const int top = codec::groupChildOf(li.groups, rl.group, -1);
            QVector<int> run;
            while (k < li.layers.size() && li.layers[k].group >= 0 && codec::groupChildOf(li.groups, li.layers[k].group, -1) == top) run << k++;
            --k;
            if (top < 0 || top >= li.groups.size()) continue;
            VObject grp = groupFromLayers(d, li, run, top, 0);
            LayerInfo L;
            L.id = d->newId();
            L.name = grp.name;
            L.visible = true;
            d->s.layers << L;
            QVector<VObject> objs;
            if (!grp.children.isEmpty()) objs << grp;
            f.layers << objs;
            continue;
        }
        // the bottom "Canvas" layer Libreworks writes (one solid colour, full size) is the canvas colour
        if (k == 0 && rl.group < 0 && rl.name == QLatin1String(kCanvasLayer) && rl.offset.isNull() && rl.image.size() == QSize(d->s.width, d->s.height) &&
            rl.opacity == 255 && rl.blend == 0 && rl.visible) {
            const QImage c = rl.image.convertToFormat(QImage::Format_ARGB32);
            const QRgb first = c.pixel(0, 0);
            bool solid = true;
            for (int y = 0; y < c.height() && solid; ++y) {
                const QRgb *r = reinterpret_cast<const QRgb *>(c.constScanLine(y));
                for (int x = 0; x < c.width(); ++x) if (r[x] != first) { solid = false; break; }
            }
            if (solid) { d->s.canvasColor = QColor::fromRgba(first); continue; }
        }
        LayerInfo L;
        L.id = d->newId();
        L.name = rl.name.isEmpty() ? QStringLiteral("Layer %1").arg(d->s.layers.size() + 1) : rl.name;
        L.visible = rl.visible;
        d->s.layers << L;
        QVector<VObject> objs;
        if (!rl.image.isNull()) objs << bitmapFromLayer(d, rl, L.name);
        f.layers << objs;
    }
    if (d->s.layers.isEmpty()) {
        // no layer records: the composite is the only image
        LayerInfo L; L.id = d->newId(); L.name = QStringLiteral("Background");
        d->s.layers << L;
        QVector<VObject> objs;
        if (!li.composite.isNull()) {
            VObject o; o.id = d->newId(); o.type = ObjType::Bitmap; o.name = QStringLiteral("Bitmap");
            o.image = li.composite.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            o.rect = QRectF(QPointF(0, 0), li.composite.size());
            o.fillOn = o.strokeOn = false;
            o.stampInterpolation();
            pxedit::trimToAlpha(o);
            objs << o;
        }
        f.layers << objs;
    }
    LayerInfo web; web.id = d->newId(); web.name = QStringLiteral("Web Layer"); web.web = web.shared = true;
    d->s.layers << web;
    f.layers.resize(d->s.layers.size());
    d->s.frames << f;
    d->resetHistory(QStringLiteral("Open"));
    if (notes) *notes << li.notes;
    return d;
}
}

Document *openData(const QByteArray &data, Fmt fmt, const QString &baseDir, QString *err, QStringList *notes) {
    if (fmt == Fmt::Unknown) fmt = ImageIO::detect(data);
    if (fmt == Fmt::PNG && isEditablePng(data)) {
        const QByteArray lw = codec::pngChunk(data, "lwDC");
        Document *d = lwd::deserialize(lw, err);
        if (d) d->resetHistory(QStringLiteral("Open"));
        return d;
    }
    if (fmt == Fmt::PSD || fmt == Fmt::ORA || fmt == Fmt::XCF) {
        codec::LayeredImage li;
        const bool ok = fmt == Fmt::PSD ? codec::readPsd(data, li, err) : fmt == Fmt::ORA ? codec::readOra(data, li, err) : codec::readXcf(data, li, err);
        if (!ok) return nullptr;
        return fromLayered(li, notes);
    }
    if (fmt == Fmt::SVG || fmt == Fmt::SVGZ) {
        auto *d = new Document;
        if (!svgToState(data, d->s, [d]() { return d->newId(); }, baseDir, notes, err)) { delete d; return nullptr; }
        d->resetHistory(QStringLiteral("Open"));
        return d;
    }
    // flat formats
    ImageSeq seq;
    if (!ImageIO::readData(data, seq, err)) return nullptr;
    if (fmt == Fmt::PNG && codec::isFireworksPng(data) && notes)
        *notes << QStringLiteral("Macromedia Fireworks editable data (mkBF / mkTS chunks) uses an undocumented format: the flattened image was opened");
    return Document::fromImageSeq(seq);
}

Document *open(const QString &path, QString *err, QStringList *notes) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { setErr(err, f.errorString()); return nullptr; }
    const QByteArray data = f.readAll();
    Fmt fmt = ImageIO::detect(data);
    const Fmt byExt = ImageIO::fromExtension(path);
    if (fmt == Fmt::Unknown) fmt = byExt;
    Document *d = openData(data, fmt, QFileInfo(path).absolutePath(), err, notes);
    if (d && fmt == Fmt::PNG && isEditablePng(data)) d->filePath.clear();   // Save goes to .lwd
    return d;
}

namespace {
bool plainGroup(const VObject &o) {
    if (o.type != ObjType::Group || o.hasMask() || o.isWeb()) return false;
    for (const auto &e : o.effects) if (e.enabled) return false;
    return true;
}
codec::RasterLayer objectLayer(const VObject &o0, const QRect &canvas) {
    VObject o = o0;
    o.visible = true;
    codec::RasterLayer rl;
    rl.name = o.name.isEmpty() ? o.displayName() : o.name;
    rl.visible = o0.visible;
    bool fx = false;
    for (const auto &e : o.effects) fx |= e.enabled;
    if (!fx) {   // opacity / blend as layer properties
        rl.opacity = qRound(o.opacity * 2.55);
        rl.blend = int(o.blend);
        o.opacity = 100;
        o.blend = BlendMode::Normal;
    }
    QRect area;
    const QImage img = render::renderObject(o, &area, canvas);
    if (!img.isNull() && !area.isEmpty()) { rl.image = img.convertToFormat(QImage::Format_ARGB32); rl.offset = area.topLeft(); }
    else { rl.image = QImage(1, 1, QImage::Format_ARGB32); rl.image.fill(0); }
    return rl;
}
void groupToLayers(codec::LayeredImage &li, const VObject &g, int parent, bool visible, const QRect &canvas, int depth) {
    codec::LayerGroup G;
    G.name = g.name.isEmpty() ? QStringLiteral("Group") : g.name;
    G.opacity = qRound(g.opacity * 2.55);
    G.blend = int(g.blend);
    G.visible = visible && g.visible;
    G.parent = parent;
    const int gi = int(li.groups.size());
    li.groups << G;
    for (const VObject &c : g.children) {
        if (c.isWeb()) continue;
        if (plainGroup(c) && depth < 32) { groupToLayers(li, c, gi, true, canvas, depth + 1); continue; }
        codec::RasterLayer rl = objectLayer(c, canvas);
        rl.group = gi;
        li.layers << rl;
    }
}
}

codec::LayeredImage layersFromState(const DocState &st, int frame) {
    codec::LayeredImage li;
    li.width = st.width;
    li.height = st.height;
    if (frame < 0 || frame >= st.frames.size()) return li;
    const Frame &f = st.frames[frame];
    if (st.canvasColor.alpha() > 0) {
        // the canvas colour as a bottom layer (Photoshop / GIMP / Krita have no canvas colour)
        codec::RasterLayer cl;
        cl.name = QString::fromLatin1(kCanvasLayer);
        cl.image = QImage(st.width, st.height, QImage::Format_ARGB32);
        cl.image.fill(st.canvasColor.rgba());
        li.layers << cl;
    }
    for (int i = 0; i < st.layers.size() && i < f.layers.size(); ++i) {
        const LayerInfo &L = st.layers[i];
        if (L.web) continue;
        // a layer holding one plain group (no mask, no live effects) is written as a layer group whose
        // members are rendered one by one (v0.3.2)
        if (f.layers[i].size() == 1 && plainGroup(f.layers[i][0])) {
            groupToLayers(li, f.layers[i][0], -1, L.visible, QRect(0, 0, st.width, st.height), 0);
            continue;
        }
        // a layer holding exactly one object keeps that object's opacity and blend mode as layer properties
        DocState one = st;
        for (int j = 0; j < one.layers.size(); ++j) one.layers[j].visible = j == i;
        int opacity = 255, blend = 0;
        if (f.layers[i].size() == 1 && f.layers[i][0].effects.isEmpty()) {
            VObject &o = one.frames[frame].layers[i][0];
            opacity = qRound(o.opacity * 2.55);
            blend = int(o.blend);
            o.opacity = 100;
            o.blend = BlendMode::Normal;
        }
        const QImage full = render::renderFrame(one, frame, false).convertToFormat(QImage::Format_ARGB32);
        // trim to the opaque bounds
        int x0 = full.width(), y0 = full.height(), x1 = -1, y1 = -1;
        for (int y = 0; y < full.height(); ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(full.constScanLine(y));
            for (int x = 0; x < full.width(); ++x)
                if (qAlpha(s[x])) { x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = qMax(y1, y); }
        }
        codec::RasterLayer rl;
        rl.name = L.name;
        rl.visible = L.visible;
        rl.opacity = opacity;
        rl.blend = blend;
        if (x1 >= 0) { rl.image = full.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1); rl.offset = QPoint(x0, y0); }
        else { rl.image = QImage(1, 1, QImage::Format_ARGB32); rl.image.fill(0); }
        li.layers << rl;
    }
    li.composite = render::renderFrame(st, frame, true).convertToFormat(QImage::Format_ARGB32);
    return li;
}

bool writePdf(const DocState &st, QByteArray &out, QString *err) {
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QPdfWriter pdf(&buf);
    pdf.setResolution(72);   // 1 document pixel = 1 pt
    pdf.setCreator(QStringLiteral("%1 %2").arg(QStringLiteral(LW_PRODUCT_NAME), QStringLiteral(LW_VERSION)));
    pdf.setTitle(QStringLiteral("%1 document").arg(QStringLiteral(LW_PRODUCT_NAME)));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setPageSize(QPageSize(QSizeF(st.width, st.height), QPageSize::Point, QString(), QPageSize::ExactMatch));
    QPainter p;
    if (!p.begin(&pdf)) { setErr(err, QStringLiteral("PDF: could not start the PDF writer")); return false; }
    SymbolScope scope(&st.symbols);
    const QRect clip(0, 0, st.width, st.height);
    for (int fi = 0; fi < qMax(1, int(st.frames.size())); ++fi) {
        if (fi > 0) pdf.newPage();
        p.setClipRect(clip);
        if (st.canvasColor.alpha() > 0) p.fillRect(clip, st.canvasColor);
        if (fi >= st.frames.size()) break;
        const Frame &f = st.frames[fi];
        for (int li = 0; li < st.layers.size() && li < f.layers.size(); ++li) {
            if (!st.layers[li].visible || st.layers[li].web) continue;
            for (const VObject &o : f.layers[li]) {
                if (!o.visible || o.opacity <= 0 || o.isWeb()) continue;
                bool fx = false;
                for (const auto &e : o.effects) fx |= e.enabled;
                if (!fx && o.blend == BlendMode::Normal) {
                    // vectors stay vectors (text as text, paths as paths); opacity is a PDF constant alpha
                    p.save();
                    p.setOpacity(o.opacity / 100.0);
                    render::drawBase(p, o);
                    p.restore();
                } else {
                    render::drawObject(p, o, clip);   // live effects / blend modes: rendered pixels
                }
            }
        }
    }
    p.end();
    return true;
}

bool exportDocument(const Document &doc, int frame, const ExportOptions &o, QByteArray &out, QString *err, QStringList *notes) {
    const DocState &st0 = doc.s;
    // Export Area: the vector / layered formats get a copy of the document cut to the area (objects
    // moved, canvas resized); the raster path renders the whole canvas and ImageIO crops it.
    DocState areaState;
    const bool docFmt = o.format == Fmt::SVG || o.format == Fmt::SVGZ || o.format == Fmt::PDF || o.format == Fmt::PSD
                        || o.format == Fmt::ORA || o.format == Fmt::XCF;
    if (docFmt && !o.area.isEmpty()) {
        const QRect a = o.area.normalized() & QRect(0, 0, st0.width, st0.height);
        if (a.isEmpty()) { setErr(err, QStringLiteral("The export area lies outside the canvas")); return false; }
        areaState = st0;
        areaState.width = a.width();
        areaState.height = a.height();
        if (!a.topLeft().isNull())
            for (auto &f : areaState.frames) for (auto &l : f.layers) for (auto &ob : l) ob.moveBy(-a.topLeft());
    }
    const DocState &st = docFmt && !o.area.isEmpty() ? areaState : st0;
    frame = qBound(0, frame, qMax(0, int(st.frames.size()) - 1));
    switch (o.format) {
    case Fmt::SVG: out = stateToSvg(st, frame, nullptr, notes); return true;
    case Fmt::SVGZ: out = gzip(stateToSvg(st, frame, nullptr, notes)); if (out.isEmpty()) { setErr(err, QStringLiteral("SVGZ: compression failed")); return false; } return true;
    case Fmt::PDF: return writePdf(st, out, err);
    case Fmt::PSD: case Fmt::ORA: case Fmt::XCF: {
        const codec::LayeredImage li = layersFromState(st, frame);
        if (o.format == Fmt::PSD) return codec::writePsd(out, li, err);
        if (o.format == Fmt::ORA) return codec::writeOra(out, li, err);
        return codec::writeXcf(out, li, err);
    }
    default: break;
    }
    ImageSeq seq;
    seq.width = st.width;
    seq.height = st.height;
    seq.loops = st.loops;
    SymbolScope scope(&st.symbols);
    if (o.animated && ImageIO::canAnimate(o.format)) {
        for (int i = 0; i < st.frames.size(); ++i) seq.frames << AnimFrame{render::renderFrame(st, i, true), st.frames[i].delayMs};
    } else seq.frames << AnimFrame{render::renderFrame(st, frame, true), st.frames.value(frame).delayMs};
    ExportOptions od = o;
    if (od.dpi <= 0 && st.dpi != 72) od.dpi = st.dpi;
    if (!ImageIO::writeData(out, seq, od, err)) return false;
    if (o.format == Fmt::PNG && o.pngEditable) {
        if (o.tinyPng && notes) *notes << QStringLiteral("editable PNG: the document is stored losslessly; the preview image is tiny-compressed");
        out = codec::pngWithChunk(out, "lwDC", lwd::serialize(doc));
        if (out.isEmpty()) { setErr(err, QStringLiteral("PNG: could not embed the document")); return false; }
    }
    return true;
}

bool exportDocumentFile(const Document &doc, int frame, const ExportOptions &o, const QString &path, QString *err, QStringList *notes) {
    QByteArray data;
    if (!exportDocument(doc, frame, o, data, err, notes)) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) { setErr(err, f.errorString()); return false; }
    return true;
}

// ======================================================================= favicon bundle
QImage faviconImage(const DocState &st, int frame, int size, int paddingPct, const QColor &bg) {
    SymbolScope scope(&st.symbols);
    // the canvas colour is part of the artwork only when it is opaque
    const QImage art = render::renderFrame(st, qBound(0, frame, qMax(0, int(st.frames.size()) - 1)), st.canvasColor.alpha() == 255);
    QImage out(size, size, QImage::Format_ARGB32_Premultiplied);
    out.fill(bg.isValid() ? bg : QColor(Qt::transparent));
    const int pad = qRound(size * qBound(0, paddingPct, 40) / 100.0);
    const QRect box(pad, pad, size - 2 * pad, size - 2 * pad);
    const QSize fit = art.size().scaled(box.size(), Qt::KeepAspectRatio);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect dst(box.x() + (box.width() - fit.width()) / 2, box.y() + (box.height() - fit.height()) / 2, fit.width(), fit.height());
    // downscale in steps for a sharper small icon
    QImage src = art;
    while (src.width() > dst.width() * 2 && src.height() > dst.height() * 2)
        src = src.scaled(src.size() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    p.drawImage(dst, src);
    p.end();
    return out.convertToFormat(QImage::Format_ARGB32);
}

bool exportFaviconBundle(const DocState &st, int frame, const QString &dirPath, const FaviconOptions &o, FaviconResult *res, QString *err) {
    QDir dir(dirPath);
    if (!dir.exists() && !QDir().mkpath(dirPath)) { setErr(err, QStringLiteral("cannot create folder %1").arg(dirPath)); return false; }
    FaviconResult r;
    auto save = [&](const QString &name, const QByteArray &data) {
        QSaveFile f(dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) { setErr(err, QStringLiteral("%1: %2").arg(name, f.errorString())); return false; }
        r.files << name;
        return true;
    };
    auto png = [&](const QImage &img) {
        QByteArray b;
        ExportOptions po;
        po.format = Fmt::PNG;
        po.pngType = 0;
        po.pngCompression = 9;
        codec::writePng(b, img, po, nullptr);
        return b;
    };
    // favicon.ico: each size rendered from the document (sharper than scaling one bitmap)
    {
        static const int icoSizes[] = {16, 32, 48};
        QVector<QImage> imgs;
        for (int i = 0; i < 3; ++i) if (o.icoSizes & (1 << i)) imgs << faviconImage(st, frame, icoSizes[i]);
        if (imgs.isEmpty()) imgs << faviconImage(st, frame, 32);
        // ICONDIR + entries + 32-bit DIBs with AND masks (the classic, universally readable form)
        QByteArray ico;
        auto u16 = [&](int v) { ico.append(char(v & 0xFF)); ico.append(char((v >> 8) & 0xFF)); };
        auto u32 = [&](quint32 v) { for (int k = 0; k < 4; ++k) ico.append(char((v >> (8 * k)) & 0xFF)); };
        QVector<QByteArray> dibs;
        for (const QImage &im : imgs) dibs << codec::writeIconDib(im);
        u16(0); u16(1); u16(int(imgs.size()));
        quint32 off = 6 + 16 * quint32(imgs.size());
        for (int i = 0; i < imgs.size(); ++i) {
            ico.append(char(imgs[i].width() & 0xFF)); ico.append(char(imgs[i].height() & 0xFF));
            ico.append(char(0)); ico.append(char(0));
            u16(1); u16(32); u32(quint32(dibs[i].size())); u32(off);
            off += quint32(dibs[i].size());
        }
        for (const QByteArray &d : dibs) ico += d;
        if (!save(QStringLiteral("favicon.ico"), ico)) return false;
    }
    if (o.svg && isVectorDocument(st, frame)) {
        if (!save(QStringLiteral("favicon.svg"), stateToSvg(st, frame))) return false;
        r.wroteSvg = true;
    }
    const QColor appleBg = o.appleBackground.isValid() ? o.appleBackground : QColor(Qt::white);
    if (!save(QStringLiteral("apple-touch-icon.png"), png(faviconImage(st, frame, 180, o.applePadding, appleBg)))) return false;
    if (!save(QStringLiteral("android-chrome-192x192.png"), png(faviconImage(st, frame, 192)))) return false;
    if (!save(QStringLiteral("android-chrome-512x512.png"), png(faviconImage(st, frame, 512)))) return false;
    QString base = o.basePath.isEmpty() ? QStringLiteral("/") : o.basePath;
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    const QString name = o.appName.isEmpty() ? QStringLiteral("App") : o.appName;
    const QString theme = (o.themeColor.isValid() ? o.themeColor : QColor(Qt::white)).name();
    {
        QJsonObject m;
        m[QStringLiteral("name")] = name;
        m[QStringLiteral("short_name")] = name.left(12);
        QJsonArray icons;
        for (int s : {192, 512}) {
            QJsonObject ic;
            ic[QStringLiteral("src")] = base + QStringLiteral("android-chrome-%1x%1.png").arg(s);
            ic[QStringLiteral("sizes")] = QStringLiteral("%1x%1").arg(s);
            ic[QStringLiteral("type")] = QStringLiteral("image/png");
            icons.append(ic);
        }
        m[QStringLiteral("icons")] = icons;
        m[QStringLiteral("theme_color")] = theme;
        m[QStringLiteral("background_color")] = theme;
        m[QStringLiteral("display")] = QStringLiteral("standalone");
        if (!save(QStringLiteral("site.webmanifest"), QJsonDocument(m).toJson(QJsonDocument::Indented))) return false;
    }
    QStringList links;
    links << QStringLiteral("<link rel=\"icon\" href=\"%1favicon.ico\" sizes=\"any\">").arg(base);
    if (r.wroteSvg) links << QStringLiteral("<link rel=\"icon\" href=\"%1favicon.svg\" type=\"image/svg+xml\">").arg(base);
    links << QStringLiteral("<link rel=\"apple-touch-icon\" href=\"%1apple-touch-icon.png\">").arg(base);
    links << QStringLiteral("<link rel=\"manifest\" href=\"%1site.webmanifest\">").arg(base);
    links << QStringLiteral("<meta name=\"theme-color\" content=\"%1\">").arg(theme);
    r.snippet = links.join(QLatin1Char('\n')) + QLatin1Char('\n');
    if (!save(QStringLiteral("favicon-snippet.html"), r.snippet.toUtf8())) return false;
    if (res) *res = r;
    return true;
}
}
