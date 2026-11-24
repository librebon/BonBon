#include "Brand.h"
#include "HtmlExport.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace html {
namespace {
QString esc(const QString &s) { return s.toHtmlEscaped(); }

QString sanitize(const QString &s) {
    QString out;
    for (QChar c : s) out += (c.isLetterOrNumber() || c == '_' || c == '-') ? c : QChar('_');
    return out;
}

const QVector<VObject> *webObjects(const Document &doc) {
    int w = doc.webLayer();
    if (w < 0 || doc.s.frames.isEmpty()) return nullptr;
    return &doc.s.frames[0].layers[w];
}

QRect sliceRect(const VObject &o, const QSize &canvas) {
    QRectF r = o.bounds();
    QRect ir(QPoint(qRound(r.left()), qRound(r.top())), QPoint(qRound(r.right()) - 1, qRound(r.bottom()) - 1));
    return ir.intersected(QRect(QPoint(0, 0), canvas));
}
}

Plan plan(const Document &doc, const QString &baseName) {
    Plan p;
    const QSize canvas(doc.s.width, doc.s.height);
    QVector<QRect> rects;
    if (const auto *objs = webObjects(doc)) {
        for (const VObject &o : *objs) {
            if (o.type != ObjType::Slice || !o.visible) continue;
            QRect r = sliceRect(o, canvas);
            if (r.isEmpty()) continue;
            bool overlap = false;
            for (const QRect &e : rects) overlap |= e.intersects(r);
            if (overlap) { p.warnings << QStringLiteral("Slice '%1' overlaps another slice and was ignored").arg(o.displayName()); continue; }
            rects << r;
            p.slices << &o;
        }
    }
    QVector<int> xs{0, canvas.width()}, ys{0, canvas.height()};
    for (const QRect &r : rects) { xs << r.left() << r.right() + 1; ys << r.top() << r.bottom() + 1; }
    std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    p.xs = xs; p.ys = ys;
    const int nc = xs.size() - 1, nr = ys.size() - 1;
    QVector<int> owner(nc * nr, -1);
    for (int si = 0; si < rects.size(); ++si)
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < nc; ++c)
                if (rects[si].contains(QRect(xs[c], ys[r], xs[c + 1] - xs[c], ys[r + 1] - ys[r]))) owner[r * nc + c] = si;
    QSet<QString> used;
    auto unique = [&](QString n) {
        QString b = n; int k = 2;
        while (used.contains(n)) n = QStringLiteral("%1_%2").arg(b).arg(k++);
        used.insert(n);
        return n;
    };
    QVector<bool> done(nc * nr, false);
    for (int r = 0; r < nr; ++r) {
        for (int c = 0; c < nc; ++c) {
            if (done[r * nc + c]) continue;
            Region g;
            g.row = r; g.col = c;
            int si = owner[r * nc + c];
            if (si >= 0) {
                const QRect &sr = rects[si];
                int c2 = c, r2 = r;
                while (c2 + 1 < nc && xs[c2 + 1] <= sr.right()) ++c2;
                while (r2 + 1 < nr && ys[r2 + 1] <= sr.bottom()) ++r2;
                g.colSpan = c2 - c + 1; g.rowSpan = r2 - r + 1;
                g.slice = si;
                g.rect = sr;
                QString n = sanitize(p.slices[si]->name);
                g.name = unique(n.isEmpty() ? QStringLiteral("%1_r%2_c%3").arg(baseName).arg(r + 1).arg(c + 1) : n);
            } else {
                int c2 = c; // merge consecutive automatic cells of one row
                while (c2 + 1 < nc && owner[r * nc + c2 + 1] < 0 && !done[r * nc + c2 + 1]) ++c2;
                g.colSpan = c2 - c + 1;
                g.rect = QRect(xs[c], ys[r], xs[c2 + 1] - xs[c], ys[r + 1] - ys[r]);
                g.name = unique(QStringLiteral("%1_r%2_c%3").arg(baseName).arg(r + 1).arg(c + 1));
            }
            for (int rr = g.row; rr < g.row + g.rowSpan; ++rr)
                for (int cc = g.col; cc < g.col + g.colSpan; ++cc) done[rr * nc + cc] = true;
            p.regions << g;
        }
    }
    return p;
}

// Button symbol instances on frame 1 become slices whose Over / Down states are swapped in by
// JavaScript (Fireworks-style buttons). Returns slice id -> instance id.
static QHash<quint64, quint64> addButtonSlices(Document &work) {
    QHash<quint64, quint64> out;
    if (work.s.frames.isEmpty()) return out;
    work.curFrame = 0;
    SymbolScope scope(&work.s.symbols);
    QVector<VObject> slices;
    for (int li = 0; li < work.s.layers.size(); ++li) {
        if (work.s.layers[li].web || !work.s.layers[li].visible) continue;
        for (const VObject &o : work.s.frames[0].layers[li]) {
            if (o.type != ObjType::Instance || !o.visible) continue;
            const Symbol *sym = work.symbol(o.symbolId);
            if (!sym || sym->kind != SymbolKind::Button) continue;
            VObject sl;
            sl.type = ObjType::Slice;
            sl.rect = QRectF(o.visualBounds().toAlignedRect());
            sl.name = o.name.isEmpty() ? sym->name : o.name;
            sl.link = o.link; sl.alt = o.alt; sl.target = o.target;
            sl.symbolId = o.id; // remember the instance temporarily
            slices << sl;
        }
    }
    if (slices.isEmpty()) return out;
    const int web = work.ensureWebLayer();
    for (VObject sl : slices) {
        const quint64 inst = sl.symbolId;
        sl.symbolId = 0;
        sl.id = work.newId();
        work.s.frames[0].layers[web].append(sl);
        out.insert(sl.id, inst);
    }
    return out;
}

QString areaTag(const VObject &h, const QPointF &off) {
    QString shape, coords;
    if (h.type == ObjType::Hotspot && h.webShape == WebShape::Circle) {
        QRectF r = h.rect.normalized().translated(-off);
        shape = QStringLiteral("circle");
        coords = QStringLiteral("%1,%2,%3").arg(qRound(r.center().x())).arg(qRound(r.center().y())).arg(qRound(qMin(r.width(), r.height()) / 2));
    } else if (h.isPolygonWeb()) {
        shape = QStringLiteral("poly");
        QStringList c;
        for (const QPointF &pt : h.points) c << QString::number(qRound(pt.x() - off.x())) << QString::number(qRound(pt.y() - off.y()));
        coords = c.join(',');
    } else {
        QRectF r = h.rect.normalized().translated(-off);
        shape = QStringLiteral("rect");
        coords = QStringLiteral("%1,%2,%3,%4").arg(qRound(r.left())).arg(qRound(r.top())).arg(qRound(r.right())).arg(qRound(r.bottom()));
    }
    QString a = QStringLiteral("<area shape=\"%1\" coords=\"%2\" href=\"%3\" alt=\"%4\"").arg(shape, coords, esc(h.link.isEmpty() ? QStringLiteral("#") : h.link), esc(h.alt));
    if (!h.target.isEmpty()) a += QStringLiteral(" target=\"%1\"").arg(esc(h.target));
    return a + QStringLiteral(">");
}

bool exportSlices(const Document &doc, const QString &dirPath, QString *err, QStringList *written) {
    QDir dir(dirPath);
    if (!dir.mkpath(QStringLiteral("."))) { if (err) *err = QStringLiteral("cannot create %1").arg(dirPath); return false; }
    const QVector<VObject> *web = webObjects(doc);
    const QSize canvas(doc.s.width, doc.s.height);
    QVector<QImage> frames;
    QSet<QString> used;
    int n = 0;
    if (web) for (const VObject &o : *web) {
        if (o.type != ObjType::Slice || !o.visible) continue;
        const QRect r = sliceRect(o, canvas);
        if (r.isEmpty()) continue;
        if (frames.isEmpty()) for (int i = 0; i < doc.s.frames.size(); ++i) frames << doc.render(i, true);
        QString name = sanitize(o.name.isEmpty() ? QStringLiteral("slice_%1").arg(n + 1) : o.name);
        QString u = name;
        for (int k = 2; used.contains(u.toLower()); ++k) u = QStringLiteral("%1_%2").arg(name).arg(k);
        used.insert(u.toLower());
        ExportOptions e = o.sliceCustomOpt ? o.sliceOpt : doc.exportOptions;
        if (!ImageIO::available(e.format)) e.format = Fmt::PNG;
        const bool anim = e.animated && frames.size() > 1 && ImageIO::canAnimate(e.format);
        e.animated = anim;
        ImageSeq seq;
        seq.width = r.width(); seq.height = r.height(); seq.loops = doc.s.loops;
        if (anim) for (int i = 0; i < frames.size(); ++i) seq.frames.append({frames[i].copy(r), doc.s.frames[i].delayMs});
        else seq.frames.append({frames[0].copy(r), 100});
        const QString file = dir.filePath(u + QLatin1Char('.') + ImageIO::extension(e.format));
        QString why;
        if (!ImageIO::write(file, seq, e, &why)) { if (err) *err = QStringLiteral("%1: %2").arg(file, why); return false; }
        if (written) *written << file;
        ++n;
    }
    if (!n) { if (err) *err = QStringLiteral("The document has no visible slices."); return false; }
    return true;
}

bool exportImageMap(const Document &doc, const QString &htmlPath, const Options &o, QString *err, QStringList *written) {
    QFileInfo hi(htmlPath);
    const QString base = sanitize(hi.completeBaseName().isEmpty() ? QStringLiteral("page") : hi.completeBaseName());
    QDir dir = hi.absoluteDir();
    if (!o.imagesDir.isEmpty() && !dir.mkpath(o.imagesDir)) { if (err) *err = QStringLiteral("cannot create %1").arg(dir.filePath(o.imagesDir)); return false; }
    ExportOptions e = doc.exportOptions;
    if (!ImageIO::available(e.format)) e.format = Fmt::PNG;
    e.animated = false;
    const QString rel = (o.imagesDir.isEmpty() ? QString() : o.imagesDir + QLatin1Char('/')) + base + QLatin1Char('.') + ImageIO::extension(e.format);
    ImageSeq seq;
    seq.width = doc.s.width; seq.height = doc.s.height;
    seq.frames.append({doc.render(0, true), 100});
    QString why;
    if (!ImageIO::write(dir.filePath(rel), seq, e, &why)) { if (err) *err = QStringLiteral("%1: %2").arg(rel, why); return false; }
    QStringList areas;
    if (const QVector<VObject> *web = webObjects(doc)) {
        for (const VObject &h : *web) if (h.type == ObjType::Hotspot && h.visible) areas << areaTag(h);
        for (const VObject &s : *web) if (s.type == ObjType::Slice && s.visible && !s.link.isEmpty()) areas << areaTag(s);
    }
    const QString title = o.title.isEmpty() ? doc.title() : o.title;
    QString h = QStringLiteral("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>%1</title>\n"
                               "<!-- Generated by %2 %3 -->\n</head>\n<body>\n").arg(esc(title), QStringLiteral(LW_PRODUCT_NAME), QStringLiteral(LW_VERSION));
    h += QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\" alt=\"%4\" usemap=\"#map_%5\" style=\"border:0\">\n")
             .arg(esc(rel)).arg(doc.s.width).arg(doc.s.height).arg(esc(title), base);
    h += QStringLiteral("<map name=\"map_%1\">\n").arg(base);
    for (const QString &a : areas) h += QStringLiteral("  ") + a + QLatin1Char('\n');
    h += QStringLiteral("</map>\n</body>\n</html>\n");
    QSaveFile f(htmlPath);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(h.toUtf8());
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    if (written) { written->prepend(dir.filePath(rel)); written->prepend(htmlPath); }
    return true;
}

bool exportHtml(const Document &orig, const QString &htmlPath, const Options &o, QString *err, QStringList *written) {
    Document work;
    work.s = orig.s;
    work.exportOptions = orig.exportOptions;
    const QHash<quint64, quint64> buttons = addButtonSlices(work);
    const Document &doc = work;
    QFileInfo hi(htmlPath);
    const QString base = sanitize(hi.completeBaseName().isEmpty() ? QStringLiteral("page") : hi.completeBaseName());
    QDir dir = hi.absoluteDir();
    if (!o.imagesDir.isEmpty() && !dir.mkpath(o.imagesDir)) { if (err) *err = QStringLiteral("cannot create %1").arg(dir.filePath(o.imagesDir)); return false; }
    const QString rel = o.imagesDir.isEmpty() ? QString() : o.imagesDir + QLatin1Char('/');
    Plan p = plan(doc, base);

    QVector<QImage> frames;
    for (int i = 0; i < doc.s.frames.size(); ++i) frames << doc.render(i, true);
    auto optsFor = [&](int sliceIdx) { return (sliceIdx >= 0 && p.slices[sliceIdx]->sliceCustomOpt) ? p.slices[sliceIdx]->sliceOpt : doc.exportOptions; };
    auto writeImg = [&](const QString &name, const QRect &r, int frame, const ExportOptions &eo, bool anim, QString *srcOut,
                        const QImage *still = nullptr) -> bool {
        ImageSeq seq;
        seq.width = r.width(); seq.height = r.height(); seq.loops = doc.s.loops;
        if (still) seq.frames.append({still->copy(r), 100});
        else if (anim) for (int i = 0; i < frames.size(); ++i) seq.frames.append({frames[i].copy(r), doc.s.frames[i].delayMs});
        else seq.frames.append({frames[qBound(0, frame, int(frames.size()) - 1)].copy(r), 100});
        ExportOptions e = eo;
        e.animated = anim;
        if (!ImageIO::available(e.format)) e.format = Fmt::PNG;
        const QString file = rel + name + QLatin1Char('.') + ImageIO::extension(e.format);
        QString why;
        if (!ImageIO::write(dir.filePath(file), seq, e, &why)) { if (err) *err = QStringLiteral("%1: %2").arg(file, why); return false; }
        if (written) *written << dir.filePath(file);
        *srcOut = file;
        return true;
    };

    // slices that are swapped by some behaviour (they need ids and over-state images)
    const QVector<VObject> *web = webObjects(doc);
    auto regionOfSlice = [&](quint64 id) -> int {
        for (int i = 0; i < p.regions.size(); ++i)
            if (p.regions[i].slice >= 0 && p.slices[p.regions[i].slice]->id == id) return i;
        return -1;
    };
    QVector<QString> src(p.regions.size()), elemId(p.regions.size());
    QHash<QString, QString> overSrc; // "region:frame" -> src
    QStringList preload;
    auto overImage = [&](int ri, int frame) -> QString {
        const QString key = QStringLiteral("%1:%2").arg(ri).arg(frame);
        if (overSrc.contains(key)) return overSrc[key];
        QString s;
        if (!writeImg(QStringLiteral("%1_f%2").arg(p.regions[ri].name).arg(frame + 1), p.regions[ri].rect, frame, optsFor(p.regions[ri].slice), false, &s)) return {};
        overSrc[key] = s;
        preload << s;
        elemId[ri] = QStringLiteral("lw_") + p.regions[ri].name;
        return s;
    };
    const bool multiFrame = frames.size() > 1;
    for (int ri = 0; ri < p.regions.size(); ++ri) {
        const Region &g = p.regions[ri];
        ExportOptions eo = optsFor(g.slice);
        bool isSwapped = false;
        if (web) for (const VObject &w : *web) if (w.behavior != WebBehavior::None && (g.slice >= 0) &&
                                                   (w.behavior == WebBehavior::SimpleRollover ? w.id == p.slices[g.slice]->id
                                                                                              : (w.swapTarget ? w.swapTarget : w.id) == p.slices[g.slice]->id)) isSwapped = true;
        bool anim = eo.animated && multiFrame && ImageIO::canAnimate(eo.format) && !isSwapped;
        if (!writeImg(g.name, g.rect, 0, eo, anim, &src[ri])) return false;
    }

    // behaviours: event attributes per region (slice) and per hotspot
    auto behaviorAttrs = [&](const VObject &w, int selfRegion, QString *err2) -> QString {
        Q_UNUSED(err2);
        if (w.behavior == WebBehavior::None || !multiFrame) return {};
        int target = w.behavior == WebBehavior::SimpleRollover ? selfRegion : (w.swapTarget ? regionOfSlice(w.swapTarget) : selfRegion);
        if (target < 0) return {};
        int fr = qBound(0, w.behavior == WebBehavior::SimpleRollover ? 1 : w.swapFrame, int(frames.size()) - 1);
        QString s = overImage(target, fr);
        if (s.isEmpty()) return {};
        return QStringLiteral(" onmouseover=\"lwSwap('%1','%2')\" onmouseout=\"lwRestore('%1')\"").arg(elemId[target], esc(s));
    };
    QVector<QString> regionEvents(p.regions.size());
    for (int ri = 0; ri < p.regions.size(); ++ri)
        if (p.regions[ri].slice >= 0) regionEvents[ri] = behaviorAttrs(*p.slices[p.regions[ri].slice], ri, nullptr);
    // button symbols: Over and Down state images
    for (int ri = 0; ri < p.regions.size(); ++ri) {
        if (p.regions[ri].slice < 0) continue;
        const quint64 inst = buttons.value(p.slices[p.regions[ri].slice]->id);
        if (!inst) continue;
        QString st[3];
        for (int state = 1; state <= 2; ++state) {
            VObject *io = work.find(inst);
            if (!io) break;
            const int keep = io->symFrame;
            io->symFrame = state;
            QImage full = work.render(0, true);
            io->symFrame = keep;
            QString s;
            if (!writeImg(QStringLiteral("%1_%2").arg(p.regions[ri].name, state == 1 ? QStringLiteral("over") : QStringLiteral("down")),
                          p.regions[ri].rect, 0, optsFor(p.regions[ri].slice), false, &s, &full)) return false;
            st[state] = s;
            overSrc.insert(QStringLiteral("%1:b%2").arg(ri).arg(state), s);
            preload << s;
        }
        if (st[1].isEmpty()) continue;
        elemId[ri] = QStringLiteral("lw_") + p.regions[ri].name;
        regionEvents[ri] = QStringLiteral(" onmouseover=\"lwSwap('%1','%2')\" onmouseout=\"lwRestore('%1')\""
                                          " onmousedown=\"lwSwap('%1','%3')\" onmouseup=\"lwSwap('%1','%2')\"")
                               .arg(elemId[ri], esc(st[1]), esc(st[2]));
    }

    // hotspots -> image maps on the region they overlap most
    QVector<QStringList> areas(p.regions.size());
    if (web) {
        for (const VObject &h : *web) {
            if (h.type != ObjType::Hotspot || !h.visible) continue;
            QRectF hb = h.bounds();
            int best = -1; double bestA = 0;
            for (int ri = 0; ri < p.regions.size(); ++ri) {
                QRectF in = hb.intersected(QRectF(p.regions[ri].rect));
                double a = in.width() * in.height();
                if (a > bestA) { bestA = a; best = ri; }
            }
            if (best < 0) continue;
            QString a = areaTag(h, p.regions[best].rect.topLeft());
            a.chop(1);   // reopen the tag for the behaviour attributes
            a += behaviorAttrs(h, best, nullptr);
            a += QStringLiteral(">");
            areas[best] << a;
        }
        // Polygon slices: the bounding rect is the image cell; the polygon is the map area.
        for (int ri = 0; ri < p.regions.size(); ++ri) {
            if (p.regions[ri].slice < 0) continue;
            const VObject *s = p.slices[p.regions[ri].slice];
            if (!s || !s->isPolygonWeb() || !s->visible) continue;
            if (s->link.isEmpty() && s->behavior == WebBehavior::None) continue;
            QString a = areaTag(*s, QPointF(p.regions[ri].rect.topLeft()));
            a.chop(1);
            a += behaviorAttrs(*s, ri, nullptr);
            a += QStringLiteral(">");
            areas[ri] << a;
        }
    }

    auto imgTag = [&](int ri, const QString &style) {
        const Region &g = p.regions[ri];
        const VObject *s = g.slice >= 0 ? p.slices[g.slice] : nullptr;
        QString alt = s ? s->alt : QString();
        QString t = QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\" alt=\"%4\"").arg(esc(src[ri])).arg(g.rect.width()).arg(g.rect.height()).arg(esc(alt));
        if (!elemId[ri].isEmpty()) t += QStringLiteral(" id=\"%1\"").arg(elemId[ri]);
        if (!areas[ri].isEmpty()) t += QStringLiteral(" usemap=\"#map_%1\"").arg(g.name);
        if (!style.isEmpty()) t += QStringLiteral(" style=\"%1\"").arg(style);
        t += QStringLiteral(">");
        if (s && !s->isPolygonWeb() && (!s->link.isEmpty() || !regionEvents[ri].isEmpty())) {
            QString a = QStringLiteral("<a href=\"%1\"").arg(esc(s->link.isEmpty() ? QStringLiteral("#") : s->link));
            if (!s->target.isEmpty()) a += QStringLiteral(" target=\"%1\"").arg(esc(s->target));
            t = a + regionEvents[ri] + QStringLiteral(">") + t + QStringLiteral("</a>");
        }
        return t;
    };

    QString h;
    const QString title = o.title.isEmpty() ? orig.title() : o.title;
    h += QStringLiteral("<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>%1</title>\n"
                        "<!-- Generated by %2 %3 -->\n<style>\nbody { margin: 0; background: #fff; }\n"
                        "img { display: block; border: 0; }\n</style>\n").arg(esc(title), QStringLiteral(LW_PRODUCT_NAME), QStringLiteral(LW_VERSION));
    const bool needJs = !overSrc.isEmpty();
    if (needJs) {
        h += QStringLiteral("<script>\nfunction lwSwap(id, src) { var i = document.getElementById(id); if (!i) return; if (!i.dataset.up) i.dataset.up = i.src; i.src = src; }\n"
                            "function lwRestore(id) { var i = document.getElementById(id); if (i && i.dataset.up) i.src = i.dataset.up; }\n"
                            "window.addEventListener('load', function () { [");
        QStringList q;
        for (const QString &s : preload) q << QStringLiteral("'%1'").arg(s);
        h += q.join(QStringLiteral(", "));
        h += QStringLiteral("].forEach(function (s) { new Image().src = s; }); });\n</script>\n");
    }
    h += QStringLiteral("</head>\n<body>\n");
    const int W = doc.s.width, H = doc.s.height;
    if (o.layout == Layout::Table) {
        h += QStringLiteral("<table style=\"border-collapse:collapse;width:%1px\" cellpadding=\"0\" cellspacing=\"0\" border=\"0\">\n<colgroup>").arg(W);
        for (int c = 0; c + 1 < p.xs.size(); ++c) h += QStringLiteral("<col style=\"width:%1px\">").arg(p.xs[c + 1] - p.xs[c]);
        h += QStringLiteral("</colgroup>\n");
        for (int r = 0; r + 1 < p.ys.size(); ++r) {
            h += QStringLiteral("<tr style=\"height:%1px\">\n").arg(p.ys[r + 1] - p.ys[r]);
            for (int ri = 0; ri < p.regions.size(); ++ri) {
                const Region &g = p.regions[ri];
                if (g.row != r) continue;
                h += QStringLiteral("  <td style=\"padding:0\"");
                if (g.rowSpan > 1) h += QStringLiteral(" rowspan=\"%1\"").arg(g.rowSpan);
                if (g.colSpan > 1) h += QStringLiteral(" colspan=\"%1\"").arg(g.colSpan);
                h += QStringLiteral(">") + imgTag(ri, {}) + QStringLiteral("</td>\n");
            }
            h += QStringLiteral("</tr>\n");
        }
        h += QStringLiteral("</table>\n");
    } else {
        h += QStringLiteral("<div style=\"position:relative;width:%1px;height:%2px\">\n").arg(W).arg(H);
        for (int ri = 0; ri < p.regions.size(); ++ri) {
            const QRect &r = p.regions[ri].rect;
            h += QStringLiteral("  <div style=\"position:absolute;left:%1px;top:%2px;width:%3px;height:%4px\">%5</div>\n")
                     .arg(r.left()).arg(r.top()).arg(r.width()).arg(r.height()).arg(imgTag(ri, {}));
        }
        h += QStringLiteral("</div>\n");
    }
    for (int ri = 0; ri < p.regions.size(); ++ri) {
        if (areas[ri].isEmpty()) continue;
        h += QStringLiteral("<map name=\"map_%1\">\n").arg(p.regions[ri].name);
        for (const QString &a : areas[ri]) h += QStringLiteral("  ") + a + QLatin1Char('\n');
        h += QStringLiteral("</map>\n");
    }
    h += QStringLiteral("</body>\n</html>\n");
    QSaveFile f(htmlPath);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(h.toUtf8());
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    if (written) written->prepend(htmlPath);
    return true;
}
}
