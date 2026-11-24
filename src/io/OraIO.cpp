// OpenRaster (.ora): ZIP with "mimetype" stored first, stack.xml, data/*.png, mergedimage.png and
// Thumbnails/thumbnail.png. Nested stacks are layer groups (name, opacity, composite-op, visibility).
#include "Codecs.h"
#include "Zip.h"
#include "core/Object.h"
#include <QBuffer>
#include <functional>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

namespace codec {
namespace {
struct OraOp { const char *op; BlendMode m; };
const OraOp kOraOps[] = {
    {"svg:src-over", BlendMode::Normal}, {"svg:multiply", BlendMode::Multiply}, {"svg:screen", BlendMode::Screen},
    {"svg:overlay", BlendMode::Overlay}, {"svg:darken", BlendMode::Darken}, {"svg:lighten", BlendMode::Lighten},
    {"svg:difference", BlendMode::Difference}, {"svg:color-dodge", BlendMode::ColorDodge}, {"svg:color-burn", BlendMode::ColorBurn},
    {"svg:hard-light", BlendMode::HardLight}, {"svg:soft-light", BlendMode::SoftLight}, {"svg:exclusion", BlendMode::Exclusion},
    {"svg:hue", BlendMode::Hue}, {"svg:saturation", BlendMode::Saturation},
    {"svg:color", BlendMode::Color}, {"svg:luminosity", BlendMode::Luminosity},
    {"svg:dst-out", BlendMode::Erase},
};
QByteArray pngBytes(const QImage &img) {
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return b;
}
}

bool readOra(const QByteArray &d, LayeredImage &out, QString *err) {
    QMap<QString, QByteArray> files;
    QString zerr;
    if (!zip::read(d, files, &zerr)) { setErr(err, QStringLiteral("ORA: %1").arg(zerr)); return false; }
    if (!files.value(QStringLiteral("mimetype")).startsWith("image/openraster")) { setErr(err, QStringLiteral("ORA: not an OpenRaster file")); return false; }
    const QByteArray stack = files.value(QStringLiteral("stack.xml"));
    if (stack.isEmpty()) { setErr(err, QStringLiteral("ORA: stack.xml missing")); return false; }
    QXmlStreamReader x(stack);
    out = LayeredImage();
    QVector<int> groupStack;   // -1 = the root stack
    QVector<RasterLayer> topFirst;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const auto at = x.attributes();
            if (x.name() == QLatin1String("image")) {
                out.width = at.value(QLatin1String("w")).toInt();
                out.height = at.value(QLatin1String("h")).toInt();
            } else if (x.name() == QLatin1String("stack")) {
                if (groupStack.isEmpty()) { groupStack << -1; continue; }
                LayerGroup G;
                G.name = at.value(QLatin1String("name")).toString();
                const double op = at.hasAttribute(QLatin1String("opacity")) ? at.value(QLatin1String("opacity")).toDouble() : 1.0;
                G.opacity = qBound(0, qRound(op * 255), 255);
                G.visible = at.value(QLatin1String("visibility")) != QLatin1String("hidden");
                const QString cop = at.value(QLatin1String("composite-op")).toString();
                for (const OraOp &o : kOraOps) if (cop == QLatin1String(o.op)) G.blend = int(o.m);
                G.parent = groupStack.last();
                groupStack << int(out.groups.size());
                out.groups << G;
            } else if (x.name() == QLatin1String("layer")) {
                RasterLayer L;
                L.name = at.value(QLatin1String("name")).toString();
                const QString src = at.value(QLatin1String("src")).toString();
                QImage img;
                img.loadFromData(files.value(src));
                if (img.isNull()) { out.partial = true; out.notes << QStringLiteral("layer '%1' unreadable").arg(L.name); continue; }
                L.image = img.convertToFormat(QImage::Format_ARGB32);
                L.offset = QPoint(at.value(QLatin1String("x")).toInt(), at.value(QLatin1String("y")).toInt());
                const double op = at.hasAttribute(QLatin1String("opacity")) ? at.value(QLatin1String("opacity")).toDouble() : 1.0;
                L.opacity = qBound(0, qRound(op * 255), 255);
                L.visible = at.value(QLatin1String("visibility")) != QLatin1String("hidden");
                L.group = groupStack.isEmpty() ? -1 : groupStack.last();
                const QString cop = at.value(QLatin1String("composite-op")).toString();
                if (!cop.isEmpty()) {
                    bool known = false;
                    for (const OraOp &o : kOraOps) if (cop == QLatin1String(o.op)) { L.blend = int(o.m); known = true; }
                    if (!known) { out.partial = true; out.notes << QStringLiteral("composite-op %1 shown as Normal").arg(cop); }
                }
                topFirst << L;
            }
        } else if (x.isEndElement() && x.name() == QLatin1String("stack")) {
            if (!groupStack.isEmpty()) groupStack.removeLast();
        }
    }
    if (x.hasError()) { setErr(err, QStringLiteral("ORA: stack.xml: %1").arg(x.errorString())); return false; }
    for (int i = topFirst.size() - 1; i >= 0; --i) out.layers << topFirst[i];
    QImage merged;
    merged.loadFromData(files.value(QStringLiteral("mergedimage.png")));
    if (!merged.isNull()) out.composite = merged.convertToFormat(QImage::Format_ARGB32);
    if (out.width <= 0 || out.height <= 0) {
        if (!merged.isNull()) { out.width = merged.width(); out.height = merged.height(); }
        else { setErr(err, QStringLiteral("ORA: no image size")); return false; }
    }
    if (out.layers.isEmpty()) {
        if (merged.isNull()) { setErr(err, QStringLiteral("ORA: no layers")); return false; }
        RasterLayer L; L.name = QStringLiteral("Layer 1"); L.image = out.composite; out.layers << L;
    }
    return true;
}

bool writeOra(QByteArray &out, const LayeredImage &li, QString *err) {
    if (li.width <= 0 || li.height <= 0) { setErr(err, QStringLiteral("ORA: empty image")); return false; }
    QVector<zip::Entry> e;
    e << zip::Entry{QStringLiteral("mimetype"), QByteArray("image/openraster"), false};
    QByteArray xml;
    QXmlStreamWriter w(&xml);
    w.setAutoFormatting(true);
    w.writeStartDocument();
    w.writeStartElement(QStringLiteral("image"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("0.0.5"));
    w.writeAttribute(QStringLiteral("w"), QString::number(li.width));
    w.writeAttribute(QStringLiteral("h"), QString::number(li.height));
    w.writeStartElement(QStringLiteral("stack"));
    QVector<zip::Entry> data;
    auto opOf = [](int blend) {
        QString op = QStringLiteral("svg:src-over");
        for (const OraOp &o : kOraOps) if (int(o.m) == blend) op = QLatin1String(o.op);
        return op;
    };
    std::function<void(const QVector<LayerNode> &)> emitNodes = [&](const QVector<LayerNode> &nodes) {
    for (int ni = nodes.size() - 1; ni >= 0; --ni) {   // stack.xml lists the top layer first
        if (nodes[ni].group >= 0) {
            const LayerGroup &G = li.groups[nodes[ni].group];
            w.writeStartElement(QStringLiteral("stack"));
            w.writeAttribute(QStringLiteral("name"), G.name);
            w.writeAttribute(QStringLiteral("opacity"), QString::number(G.opacity / 255.0, 'f', 3));
            w.writeAttribute(QStringLiteral("visibility"), G.visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
            w.writeAttribute(QStringLiteral("composite-op"), opOf(G.blend));
            w.writeAttribute(QStringLiteral("isolation"), QStringLiteral("isolate"));
            emitNodes(nodes[ni].children);
            w.writeEndElement();
            continue;
        }
        const int i = nodes[ni].layer;
        const RasterLayer &L = li.layers[i];
        const QString src = QStringLiteral("data/layer%1.png").arg(i);
        w.writeStartElement(QStringLiteral("layer"));
        w.writeAttribute(QStringLiteral("name"), L.name);
        w.writeAttribute(QStringLiteral("src"), src);
        w.writeAttribute(QStringLiteral("x"), QString::number(L.offset.x()));
        w.writeAttribute(QStringLiteral("y"), QString::number(L.offset.y()));
        w.writeAttribute(QStringLiteral("opacity"), QString::number(L.opacity / 255.0, 'f', 3));
        w.writeAttribute(QStringLiteral("visibility"), L.visible ? QStringLiteral("visible") : QStringLiteral("hidden"));
        w.writeAttribute(QStringLiteral("composite-op"), opOf(L.blend));
        w.writeEndElement();
        data << zip::Entry{src, pngBytes(L.image), false};   // PNG is already deflated
    }
    };
    emitNodes(layerTree(li));
    w.writeEndElement();
    w.writeEndElement();
    w.writeEndDocument();
    e << zip::Entry{QStringLiteral("stack.xml"), xml, true};
    e += data;
    const QImage comp = li.composite.isNull() ? compositeLayers(li) : li.composite;
    e << zip::Entry{QStringLiteral("mergedimage.png"), pngBytes(comp), false};
    const QImage thumb = (comp.width() > 256 || comp.height() > 256) ? comp.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation) : comp;
    e << zip::Entry{QStringLiteral("Thumbnails/thumbnail.png"), pngBytes(thumb), false};
    out = zip::write(e);
    return !out.isEmpty();
}

} // namespace codec
