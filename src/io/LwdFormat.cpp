#include "Brand.h"
#include "core/PixelScale.h"
#include "core/CorePrefs.h"
#include "LwdFormat.h"
#include "core/AutoShape.h"
#include "core/Fills.h"
#include "core/Procedural.h"
#include "Zip.h"
#include "core/Document.h"
#include "core/Render.h"
#include <QBuffer>
#include <QImageWriter>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QCache>
#include <QMutex>
#include <QtConcurrentMap>
#include <QtConcurrentRun>

namespace lwd {
namespace {
// the version of the document being read (objects pasted or loaded from styles are current)
thread_local int s_readVersion = 4;
QString col(const QColor &c) { return c.name(QColor::HexArgb); }
QColor toCol(const QJsonValue &v, const QColor &def) {
    QColor c(v.toString());
    return c.isValid() ? c : def;
}
QJsonArray pt(const QPointF &p) { return {p.x(), p.y()}; }
QPointF toPt(const QJsonValue &v) { QJsonArray a = v.toArray(); return {a.at(0).toDouble(), a.at(1).toDouble()}; }
QJsonArray rc(const QRectF &r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRectF toRc(const QJsonValue &v) { QJsonArray a = v.toArray(); return {a.at(0).toDouble(), a.at(1).toDouble(), a.at(2).toDouble(), a.at(3).toDouble()}; }

QJsonObject effectJson(const LiveEffect &e) {
    QJsonObject j{{"type", effectKey(e.type)}, {"enabled", e.enabled}, {"distance", e.distance}, {"angle", e.angle},
                  {"softness", e.softness}, {"color", col(e.color)}, {"opacity", e.opacity}, {"width", e.width}};
    switch (e.type) {
    case EffectType::InnerBevel: case EffectType::OuterBevel: j["preset"] = int(e.preset); break;
    case EffectType::InsetEmboss: case EffectType::RaisedEmboss: j["showObject"] = e.showObject; break;
    case EffectType::UnsharpMask: j["amount"] = e.amount; j["threshold"] = e.threshold; break;
    case EffectType::Noise: j["amount"] = e.amount; j["colorNoise"] = e.colorNoise; break;
    case EffectType::BrightnessContrast: j["brightness"] = e.brightness; j["contrast"] = e.contrast; break;
    case EffectType::HueSaturation:
        j["hue"] = e.hue; j["saturation"] = e.saturation; j["lightness"] = e.lightness; j["colorize"] = e.colorize; break;
    case EffectType::Levels: {
        QJsonArray a; // [inBlack, inWhite, outBlack, outWhite, gamma] for RGB, Red, Green, Blue
        for (const LevelsChannel &c : e.levels) a.append(QJsonArray{c.inBlack, c.inWhite, c.outBlack, c.outWhite, c.gamma});
        j["levels"] = a;
        break;
    }
    case EffectType::Curves: {
        QJsonArray chs;
        for (const QPolygonF &c : e.curves) {
            QJsonArray a;
            for (const QPointF &p : c) a.append(pt(p));
            chs.append(a);
        }
        j["curves"] = chs;
        break;
    }
    case EffectType::MotionBlur: break; // angle + distance
    case EffectType::RadialBlur: case EffectType::ZoomBlur: j["amount"] = e.amount; j["quality"] = e.quality; break;
    case EffectType::Threshold: j["threshold"] = e.threshold; break;
    case EffectType::Posterize: j["posterize"] = e.posterize; break;
    case EffectType::ColorBalance: {
        QJsonArray a;
        for (auto &r : e.balance) a.append(QJsonArray{r[0], r[1], r[2]});
        j["balance"] = a;
        j["preserveLuminosity"] = e.preserveLuminosity;
        break;
    }
    case EffectType::PhotoFilter: j["amount"] = e.amount; j["preserveLuminosity"] = e.preserveLuminosity; break;
    case EffectType::ColorFill: j["blend"] = blendModeKey(e.blend); break;
    case EffectType::HighPass: j["keepColor"] = e.keepColor; break;
    case EffectType::Offset: j["offsetX"] = e.offsetX; j["offsetY"] = e.offsetY; j["undefinedAreas"] = e.undefinedAreas; break;
    case EffectType::FeatherEdges: j["amount"] = e.amount; j["featherCentered"] = e.featherCentered; break;
    default: break;
    }
    if (int(e.type) >= int(EffectType::GradientMap)) {
        // v0.3.0 effects: generic parameters
        j["amount"] = e.amount; j["threshold"] = e.threshold; j["posterize"] = e.posterize; j["quality"] = e.quality;
        j["mode"] = e.mode; j["mode2"] = e.mode2; j["flag"] = e.flag; j["flag2"] = e.flag2;
        QJsonArray pa;
        for (double v : e.p) pa.append(v);
        j["p"] = pa;
        if (!e.stops.isEmpty()) {
            QJsonArray st;
            for (const GradStop &g : e.stops) st.append(QJsonArray{g.pos, col(g.color)});
            j["stops"] = st;
        }
        auto palJson = [](const QVector<QRgb> &pal) { QJsonArray a; for (QRgb c : pal) a.append(QColor::fromRgba(c).name(QColor::HexArgb)); return a; };
        if (!e.palette.isEmpty()) j["palette"] = palJson(e.palette);
        if (!e.palette2.isEmpty()) j["palette2"] = palJson(e.palette2);
    }
    if (e.type == EffectType::ReplaceColor) {
        j["color2"] = col(e.color2);
        j["hue"] = e.hue; j["saturation"] = e.saturation; j["lightness"] = e.lightness;
    }
    return j;
}
LiveEffect effectFrom(const QJsonObject &j, bool *ok) {
    LiveEffect e = LiveEffect::make(effectFromKey(j["type"].toString(), ok));
    e.enabled = j["enabled"].toBool(true);
    e.distance = j["distance"].toDouble(e.distance);
    e.angle = j["angle"].toDouble(e.angle);
    e.softness = j["softness"].toDouble(e.softness);
    e.color = toCol(j["color"], e.color);
    e.color2 = toCol(j["color2"], e.color2);
    e.opacity = j["opacity"].toInt(e.opacity);
    e.width = j["width"].toDouble(e.width);
    if (e.type == EffectType::InnerBevel && !j.contains("preset")) e.angle = 135; // v0.1 bevels: fixed top-left light
    e.preset = BevelPreset(qBound(0, j["preset"].toInt(0), 3));
    e.showObject = j["showObject"].toBool(e.showObject);
    e.amount = j["amount"].toDouble(e.amount);
    e.threshold = j["threshold"].toInt(e.threshold);
    e.colorNoise = j["colorNoise"].toBool(e.colorNoise);
    e.brightness = j["brightness"].toDouble(e.brightness);
    e.contrast = j["contrast"].toDouble(e.contrast);
    e.hue = j["hue"].toDouble(e.hue);
    e.saturation = j["saturation"].toDouble(e.saturation);
    e.lightness = j["lightness"].toDouble(e.lightness);
    e.colorize = j["colorize"].toBool(e.colorize);
    if (QJsonArray l = j["levels"].toArray(); !l.isEmpty()) {
        for (int c = 0; c < kChannels && c < l.size(); ++c) {
            QJsonArray v = l[c].toArray();
            if (v.size() < 5) continue;
            e.levels[c] = {v[0].toInt(), v[1].toInt(255), v[2].toInt(), v[3].toInt(255), v[4].toDouble(1.0)};
        }
    }
    if (QJsonArray chs = j["curves"].toArray(); !chs.isEmpty()) {
        for (int c = 0; c < kChannels && c < chs.size(); ++c) {
            e.curves[c].clear();
            for (const auto &v : chs[c].toArray()) e.curves[c].append(toPt(v));
        }
    }
    e.quality = j["quality"].toInt(e.quality);
    e.posterize = j["posterize"].toInt(e.posterize);
    if (QJsonArray b = j["balance"].toArray(); b.size() == 3)
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) e.balance[r][c] = b[r].toArray().at(c).toDouble();
    e.preserveLuminosity = j["preserveLuminosity"].toBool(e.preserveLuminosity);
    if (j.contains("blend")) e.blend = blendModeFromKey(j["blend"].toString());
    e.keepColor = j["keepColor"].toBool(e.keepColor);
    e.offsetX = j["offsetX"].toDouble(e.offsetX);
    e.offsetY = j["offsetY"].toDouble(e.offsetY);
    e.undefinedAreas = qBound(0, j["undefinedAreas"].toInt(e.undefinedAreas), 2);
    e.featherCentered = j["featherCentered"].toBool(e.featherCentered);
    e.mode = j["mode"].toInt(e.mode);
    e.mode2 = j["mode2"].toInt(e.mode2);
    e.flag = j["flag"].toBool(e.flag);
    e.flag2 = j["flag2"].toBool(e.flag2);
    if (QJsonArray pa = j["p"].toArray(); !pa.isEmpty())
        for (int i = 0; i < 8 && i < pa.size(); ++i) e.p[i] = pa[i].toDouble();
    if (QJsonArray st = j["stops"].toArray(); !st.isEmpty()) {
        e.stops.clear();
        for (const auto &v : st) { QJsonArray a = v.toArray(); e.stops.append({a.at(0).toDouble(), toCol(a.at(1), Qt::black)}); }
    }
    auto palFrom = [](const QJsonValue &v) { QVector<QRgb> pal; for (const auto &c : v.toArray()) pal << QColor(c.toString()).rgba(); return pal; };
    if (j.contains("palette")) e.palette = palFrom(j["palette"]);
    if (j.contains("palette2")) e.palette2 = palFrom(j["palette2"]);
    return e;
}

QJsonObject exportJson(const ExportOptions &e);
ExportOptions exportFrom(const QJsonObject &j);

namespace {
void pathJson(const VObject &o, QJsonObject &j) {
    QJsonArray a;
    for (const QPointF &p : o.points) a.append(pt(p));
    j["points"] = a;
    j["closed"] = o.closed;
    if (o.hasCurves()) {
        QJsonArray in, out;
        for (const QPointF &p : o.hIn) in.append(pt(p));
        for (const QPointF &p : o.hOut) out.append(pt(p));
        j["handlesIn"] = in;
        j["handlesOut"] = out;
    }
    if (!o.contours.isEmpty()) {
        QJsonArray c;
        for (int i : o.contours) c.append(i);
        j["contours"] = c;
    }
    if (o.hasPointPressure()) {   // Path Scrubber data (v0.3.1), one value per point
        QJsonArray sz, op;
        for (int i = 0; i < o.points.size(); ++i) { sz.append(o.pointSize(i)); op.append(o.pointOpacity(i)); }
        j["ptSize"] = sz;
        j["ptOpacity"] = op;
    }
    if (o.hasPointSense()) {      // raw pressure / speed (v7 optional)
        QJsonArray pr, sp;
        for (int i = 0; i < o.points.size(); ++i) { pr.append(o.pointPressure(i)); sp.append(o.pointSpeed(i)); }
        if (o.ptPressure.size() == o.points.size()) j["ptPressure"] = pr;
        if (o.ptSpeed.size() == o.points.size()) j["ptSpeed"] = sp;
    }
    if (o.ptTime.size() == o.points.size() && !o.points.isEmpty()) {
        QJsonArray tm;
        for (double v : o.ptTime) tm.append(v);
        j["ptTime"] = tm;
    }
}
void pathFrom(const QJsonObject &j, VObject &o) {
    for (const auto &p : j["points"].toArray()) o.points << toPt(p);
    o.closed = j["closed"].toBool();
    for (const auto &p : j["handlesIn"].toArray()) o.hIn << toPt(p);
    for (const auto &p : j["handlesOut"].toArray()) o.hOut << toPt(p);
    if (o.hIn.size() != o.points.size() || o.hOut.size() != o.points.size()) { o.hIn.clear(); o.hOut.clear(); }
    int last = 0;
    for (const auto &c : j["contours"].toArray()) {
        const int v = c.toInt();
        if (v > last && v < o.points.size()) { o.contours << v; last = v; }
    }
    auto values = [&](const char *key, double hi) {
        QVector<double> v;
        const QJsonArray a = j[key].toArray();
        if (a.size() != o.points.size()) return v;
        for (const auto &x : a) {
            const double d = x.toDouble(1.0);
            v << (std::isfinite(d) ? qBound(0.0, d, hi) : 1.0);
        }
        return v;
    };
    o.ptSize = values("ptSize", VObject::kMaxPointSize);
    o.ptOpacity = values("ptOpacity", 1.0);
    if (o.ptSize.isEmpty() != o.ptOpacity.isEmpty()) o.ensurePointPressure();
    o.ptPressure = values("ptPressure", 1.0);
    o.ptSpeed = values("ptSpeed", VObject::kMaxPointSpeed);
    o.ptTime = values("ptTime", VObject::kMaxPointTime);
}
} // namespace

// imageKeys: bitmaps are written as their QImage cache keys instead of PNG files (contentKey())
// P8: the PNG encodes are most of a save. The bytes of an unchanged image are reused from the last
// save (QImage::cacheKey changes whenever its pixels can change), and serialize() encodes the images
// on the thread pool, beside the preview render. The written bytes are the same as before.
QByteArray pngBytes(const QImage &img) {
    static QMutex mutex;
    static QCache<qint64, QByteArray> cache(256 * 1024);   // cost in KB of PNG data
    const qint64 key = img.cacheKey();
    {
        QMutexLocker lock(&mutex);
        if (const QByteArray *hit = cache.object(key)) return *hit;
    }
    QByteArray png;
    QBuffer b(&png);
    b.open(QIODevice::WriteOnly);
    // 0.3.15: zlib level 1 — same pixels, much faster on large bitmaps (re-save still hits the cache).
    QImageWriter w(&b, "png");
    w.setCompression(1);
    if (!w.write(img)) img.save(&b, "PNG");
    QMutexLocker lock(&mutex);
    cache.insert(key, new QByteArray(png), qMax<qsizetype>(1, png.size() / 1024));
    return png;
}

struct PendingPng { qsizetype index; QImage image; };
thread_local QVector<PendingPng> *t_pending = nullptr;   // set by serialize(): encode later, in parallel

void addPng(QVector<zip::Entry> &files, const QString &name, const QImage &img) {
    if (t_pending) {
        files.append({name, QByteArray(), false});
        t_pending->append({files.size() - 1, img});
    } else {
        files.append({name, pngBytes(img), false});
    }
}

QJsonObject objJson(const VObject &o, QVector<zip::Entry> &files, const QString &imgPrefix = QString(), bool imageKeys = false) {
    QJsonObject j{{"id", QString::number(o.id)}, {"type", objTypeKey(o.type)}, {"name", o.name},
                  {"opacity", o.opacity}, {"blend", blendModeKey(o.blend)}, {"visible", o.visible}, {"locked", o.locked}};
    if (o.quickMask) j["quickMask"] = true;
    switch (o.type) {
    case ObjType::Bitmap: {
        // Always record the method used for display (effectiveInterpolation) so content/visual
        // cache keys never alias unset (-1) bitmaps across Preferences changes (0.3.17).
        const QString interpKey = pixelscale::resampleKey(o.effectiveInterpolation());
        if (imageKeys) {
            j["rect"] = rc(o.rect);
            j["imageKey"] = QString::number(o.image.cacheKey());
            j["interpolation"] = interpKey;
            break;
        }
        QString f = QStringLiteral("images/%1%2.png").arg(imgPrefix).arg(o.id);
        addPng(files, f, o.image);
        j["rect"] = rc(o.rect);
        j["image"] = f;
        // Persist only when stamped; load path stamps missing keys from Preferences.
        if (o.interpolation >= 0) j["interpolation"] = interpKey;
        break;
    }
    case ObjType::Line: j["p1"] = pt(o.p1); j["p2"] = pt(o.p2); break;
    case ObjType::Path: pathJson(o, j); break;
    default: j["rect"] = rc(o.rect); break;
    }
    if (o.type == ObjType::Text) {
        j["text"] = o.text; j["font"] = o.fontFamily; j["fontSize"] = o.fontSize;
        j["bold"] = o.bold; j["italic"] = o.italic; j["align"] = o.textAlign;
        j["textAntialias"] = textAAKey(o.textAA);
        if (o.underline) j["underline"] = true;
        if (o.kerning != 0) j["kerning"] = o.kerning;
        if (o.leading != 100) j["leading"] = o.leading;
        if (o.hScale != 100) j["hScale"] = o.hScale;
        if (o.baselineShift != 0) j["baselineShift"] = o.baselineShift;
        if (o.textOrientation) j["orientation"] = o.textOrientation;
        if (o.textDirection) j["direction"] = o.textDirection;
        if (o.paraIndent != 0) j["indent"] = o.paraIndent;
        if (o.paraSpaceBefore != 0) j["spaceBefore"] = o.paraSpaceBefore;
        if (o.paraSpaceAfter != 0) j["spaceAfter"] = o.paraSpaceAfter;
        if (o.textOnPath) {
            QJsonObject g;
            pathJson(o, g);
            g["offset"] = o.textPathOffset;
            if (o.textPathReverse) g["reverse"] = true;
            j["onPath"] = g;
        }
    }
    if (o.type == ObjType::Rect) j["roundness"] = o.roundness;
    if (o.type == ObjType::AutoShape) {
        j["shape"] = o.shapeKind;
        QJsonObject pm;
        for (auto it = o.shapeParams.cbegin(); it != o.shapeParams.cend(); ++it) pm[it.key()] = it.value();
        j["shapeParams"] = pm;
    }
    if (o.hasXform()) {
        const QTransform &t = o.xform;
        j["transform"] = QJsonArray{t.m11(), t.m12(), t.m13(), t.m21(), t.m22(), t.m23(), t.m31(), t.m32(), t.m33()};
    }
    if (o.type == ObjType::Instance) {
        j["symbol"] = QString::number(o.symbolId);
        j["symbolFrame"] = o.symFrame;
    }
    if (o.isWeb() || (o.type == ObjType::Instance && (!o.link.isEmpty() || !o.alt.isEmpty() || !o.target.isEmpty()))) {
        if (o.isWeb()) {
            static const char *shapes[] = {"rect", "circle", "polygon"};
            j["shape"] = shapes[int(o.webShape)];
            if (o.webShape == WebShape::Polygon) {
                QJsonArray a;
                for (const QPointF &p : o.points) a.append(pt(p));
                j["points"] = a;
            }
        }
        j["link"] = o.link; j["alt"] = o.alt; j["target"] = o.target;
        static const char *beh[] = {"none", "simpleRollover", "swapImage"};
        j["behavior"] = beh[int(o.behavior)];
        if (o.swapTarget) j["swapTarget"] = QString::number(o.swapTarget);
        j["swapFrame"] = o.swapFrame;
        if (o.sliceCustomOpt) j["optimize"] = exportJson(o.sliceOpt);
    }
    if (o.isVector()) {
        QJsonObject fj{{"on", o.fillOn}, {"color", col(o.fill)}};
        if (o.fillKind != FillKind::Solid) fj["kind"] = fills::fillKindKey(o.fillKind);
        if (!o.gradient.isEmpty()) {
            QJsonArray ga;
            for (const GradStop &g : o.gradient) ga.append(QJsonObject{{"pos", g.pos}, {"color", col(g.color)}});
            fj["gradient"] = ga;
        }
        if (fills::isGradient(o.fillKind)) fj["handles"] = QJsonArray{pt(o.gradP0), pt(o.gradP1), pt(o.gradP2)};
        if (o.fillKind == FillKind::Pattern) {
            fj["pattern"] = o.pattern;
            if (!o.patternXform.isIdentity()) {   // the pattern handles (0.3.4; absent = the old look)
                const QTransform &t = o.patternXform;
                fj["patternXform"] = QJsonArray{t.m11(), t.m12(), t.m21(), t.m22(), t.dx(), t.dy()};
            }
            if (o.pattern == QLatin1String("Custom") && !o.patternImage.isNull() && imageKeys) {
                fj["patternImageKey"] = QString::number(o.patternImage.cacheKey());
            } else if (o.pattern == QLatin1String("Custom") && !o.patternImage.isNull()) {
                const QString f = QStringLiteral("images/%1pattern%2.png").arg(imgPrefix).arg(o.id);
                addPng(files, f, o.patternImage);
                fj["patternImage"] = f;
            }
        }
        if (!o.texture.isEmpty()) { fj["texture"] = o.texture; fj["textureAmount"] = o.textureAmount; }
        if (!o.textureProc.isEmpty()) fj["textureProc"] = QJsonDocument::fromJson(o.textureProc.toUtf8()).object();
        if (!o.patternProc.isEmpty()) fj["patternProc"] = QJsonDocument::fromJson(o.patternProc.toUtf8()).object();
        if (o.feather > 0) fj["feather"] = o.feather;
        j["fill"] = fj;
        QJsonObject sj{{"on", o.strokeOn}, {"color", col(o.stroke)}, {"width", o.strokeWidth}};
        if (o.strokeKind != QLatin1String("basic.hard")) sj["kind"] = o.strokeKind;
        if (o.strokeCustom) sj["edit"] = stroke::toJson(o.strokeParams);
        if (!o.strokeTexture.isEmpty()) { sj["texture"] = o.strokeTexture; sj["textureAmount"] = o.strokeTextureAmount; }
        if (!o.strokeTextureProc.isEmpty()) sj["textureProc"] = QJsonDocument::fromJson(o.strokeTextureProc.toUtf8()).object();
        if (o.strokeAlign) sj["align"] = o.strokeAlign == 1 ? QStringLiteral("inside") : QStringLiteral("outside");
        if (o.strokeSeed) sj["seed"] = double(o.strokeSeed);
        j["stroke"] = sj;
        j["antialias"] = o.antialias;
    }
    if (!o.effects.isEmpty()) {
        QJsonArray ea;
        for (const auto &e : o.effects) ea.append(effectJson(e));
        j["effects"] = ea;
    }
    if (o.type == ObjType::Group) {   // v3 (0.3.2)
        QJsonArray ca;
        for (const VObject &c : o.children) ca.append(objJson(c, files, imgPrefix, imageKeys));
        j["children"] = ca;
        if (o.hasMask()) {
            j["mask"] = QJsonObject{{"object", objJson(o.maskObj[0], files, imgPrefix, imageKeys)},
                                    {"mode", o.maskMode == VObject::MaskGrayscale ? "grayscale" : "outline"},
                                    {"enabled", o.maskEnabled}, {"linked", o.maskLinked}};
        }
    }
    return j;
}

bool objFrom(const QJsonObject &j, const QMap<QString, QByteArray> &files, VObject &o, QString *err) {
    bool ok = true;
    o.id = j["id"].toString().toULongLong();
    o.type = objTypeFromKey(j["type"].toString(), &ok);
    if (!ok) { if (err) *err = QStringLiteral("unknown object type '%1'").arg(j["type"].toString()); return false; }
    o.name = j["name"].toString();
    o.opacity = j["opacity"].toInt(100);
    o.blend = blendModeFromKey(j["blend"].toString());
    o.visible = j["visible"].toBool(true);
    o.locked = j["locked"].toBool(false);
    o.quickMask = j["quickMask"].toBool(false);
    if (j.contains("rect")) o.rect = toRc(j["rect"]);
    if (o.type == ObjType::Bitmap) {
        QByteArray png = files.value(j["image"].toString());
        o.image = QImage::fromData(png, "PNG").convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (o.image.isNull()) { if (err) *err = QStringLiteral("missing image %1").arg(j["image"].toString()); return false; }
        // 0.3.16: per-object interpolation. Missing key (older files) → stamp the live Preferences
        // value at load so look matches the previous render-with-live-pref behaviour for this
        // session's open, then stays frozen if the user later changes Preferences.
        if (j.contains(QStringLiteral("interpolation")))
            o.interpolation = pixelscale::resampleFromKey(j["interpolation"].toString(), coreprefs::interpolation);
        else
            o.stampInterpolation();
    }
    if (o.type == ObjType::Line) { o.p1 = toPt(j["p1"]); o.p2 = toPt(j["p2"]); }
    if (o.type == ObjType::Path) pathFrom(j, o);
    if (o.type == ObjType::Text) {
        o.text = j["text"].toString(); o.fontFamily = j["font"].toString(o.fontFamily);
        o.fontSize = j["fontSize"].toDouble(o.fontSize); o.bold = j["bold"].toBool();
        o.italic = j["italic"].toBool(); o.textAlign = j["align"].toInt();
        o.underline = j["underline"].toBool(); o.kerning = j["kerning"].toDouble(0);
        o.leading = j["leading"].toDouble(100); o.hScale = j["hScale"].toDouble(100);
        o.baselineShift = j["baselineShift"].toDouble(0);
        o.textOrientation = j["orientation"].toInt(0);
        o.textDirection = j["direction"].toInt(0);
        o.paraIndent = j["indent"].toDouble(0);
        o.paraSpaceBefore = j["spaceBefore"].toDouble(0);
        o.paraSpaceAfter = j["spaceAfter"].toDouble(0);
        if (j.contains("onPath")) {
            const QJsonObject g = j["onPath"].toObject();
            pathFrom(g, o);
            o.textPathOffset = g["offset"].toDouble();
            o.textPathReverse = g["reverse"].toBool();
            o.textOnPath = o.points.size() > 1;
        }
    }
    o.roundness = j["roundness"].toDouble(0);
    if (o.type == ObjType::AutoShape) {
        o.shapeKind = j["shape"].toString();
        if (!autoshape::isKind(o.shapeKind)) { if (err) *err = QStringLiteral("unknown auto shape '%1'").arg(o.shapeKind); return false; }
        const QJsonObject pm = j["shapeParams"].toObject();
        o.shapeParams = autoshape::defaults(o.shapeKind);
        for (auto it = pm.begin(); it != pm.end(); ++it) o.shapeParams[it.key()] = it.value().toDouble();
    }
    if (const QJsonArray ta = j["transform"].toArray(); ta.size() == 9) {
        double m[9];
        for (int i = 0; i < 9; ++i) m[i] = ta[i].toDouble();
        o.xform = QTransform(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        if (!o.usesXform()) o.xform = QTransform();
    }
    if (o.type == ObjType::Instance) {
        o.symbolId = j["symbol"].toString().toULongLong();
        o.symFrame = qMax(0, j["symbolFrame"].toInt());
        o.fillOn = o.strokeOn = false;
        o.link = j["link"].toString(); o.alt = j["alt"].toString(); o.target = j["target"].toString();
    }
    QJsonObject f = j["fill"].toObject(), s = j["stroke"].toObject();
    if (!f.isEmpty()) {
        o.fillOn = f["on"].toBool(); o.fill = toCol(f["color"], o.fill);
        o.fillKind = fills::fillKindFromKey(f["kind"].toString());
        for (const auto &g : f["gradient"].toArray()) {
            const QJsonObject go = g.toObject();
            o.gradient.append({qBound(0.0, go["pos"].toDouble(), 1.0), toCol(go["color"], Qt::black)});
        }
        const QJsonArray h = f["handles"].toArray();
        if (h.size() == 3) { o.gradP0 = toPt(h[0]); o.gradP1 = toPt(h[1]); o.gradP2 = toPt(h[2]); }
        else if (fills::isGradient(o.fillKind)) { FillKind k = o.fillKind; o.fillKind = FillKind::Solid; fills::setFillKind(o, k); }
        // before version 4 Contour ignored its handles: keep old files looking the same
        if (o.fillKind == FillKind::Contour && s_readVersion < 4) fills::resetContourHandles(o);
        o.pattern = f["pattern"].toString(o.pattern);
        {
            const QJsonArray px = f["patternXform"].toArray();
            if (px.size() == 6) {
                const QTransform t(px[0].toDouble(1), px[1].toDouble(), px[2].toDouble(), px[3].toDouble(1), px[4].toDouble(), px[5].toDouble());
                if (std::isfinite(t.determinant()) && std::abs(t.determinant()) > 1e-9) o.patternXform = t;
            }
        }
        if (f.contains("patternImage"))
            o.patternImage = QImage::fromData(files.value(f["patternImage"].toString()), "PNG").convertToFormat(QImage::Format_ARGB32_Premultiplied);
        o.texture = f["texture"].toString();
        if (f.contains("textureProc")) o.textureProc = proc::toString(proc::fromJson(f["textureProc"].toObject()));
        if (f.contains("patternProc")) o.patternProc = proc::toString(proc::fromJson(f["patternProc"].toObject()));
        o.textureAmount = qBound(0, f["textureAmount"].toInt(), 100);
        o.feather = qMax(0.0, f["feather"].toDouble());
    }
    if (!s.isEmpty()) {
        o.strokeOn = s["on"].toBool(); o.stroke = toCol(s["color"], o.stroke); o.strokeWidth = s["width"].toDouble(1);
        o.strokeKind = fills::resolveStrokeKind(s["kind"].toString(QStringLiteral("basic.hard")));
        o.strokeParams = stroke::paramsFor(o.strokeKind);
        if (s.contains("seed")) o.strokeSeed = quint32(s["seed"].toDouble());
        if (s.contains("edit") && s["edit"].isObject()) {
            o.strokeCustom = true;
            o.strokeParams = stroke::fromJson(s["edit"].toObject(), o.strokeParams);
            if (!o.strokeParams.key.isEmpty())
                o.strokeKind = fills::resolveStrokeKind(o.strokeParams.key);
        }
        o.strokeTexture = s["texture"].toString();
        o.strokeTextureAmount = qBound(0, s["textureAmount"].toInt(), 100);
        if (s.contains("textureProc")) o.strokeTextureProc = proc::toString(proc::fromJson(s["textureProc"].toObject()));
        const QString al = s["align"].toString();
        o.strokeAlign = al == QLatin1String("inside") ? 1 : al == QLatin1String("outside") ? 2 : 0;
    }
    o.antialias = j["antialias"].toBool(true);
    if (o.type == ObjType::Text) {
        // v0.1 files only had the bool; map it onto the Fireworks modes.
        bool tok = false;
        if (j.contains("textAntialias")) o.textAA = textAAFromKey(j["textAntialias"].toString(), &tok);
        if (!tok) o.textAA = o.antialias ? TextAA::Smooth : TextAA::None;
    }
    if (o.isWeb()) {
        const QString sh = j["shape"].toString();
        o.webShape = sh == QLatin1String("circle") ? WebShape::Circle : sh == QLatin1String("polygon") ? WebShape::Polygon : WebShape::Rect;
        if (o.webShape == WebShape::Polygon) {
            for (const auto &p : j["points"].toArray()) o.points << toPt(p);
            if (!o.points.isEmpty()) o.rect = o.points.boundingRect();
        }
        o.link = j["link"].toString(); o.alt = j["alt"].toString(); o.target = j["target"].toString();
        const QString b = j["behavior"].toString();
        o.behavior = b == QLatin1String("simpleRollover") ? WebBehavior::SimpleRollover : b == QLatin1String("swapImage") ? WebBehavior::SwapImage : WebBehavior::None;
        o.swapTarget = j["swapTarget"].toString().toULongLong();
        o.swapFrame = j["swapFrame"].toInt(1);
        o.sliceCustomOpt = j.contains("optimize");
        if (o.sliceCustomOpt) o.sliceOpt = exportFrom(j["optimize"].toObject());
    }
    for (const auto &e : j["effects"].toArray()) {
        bool eok = true;
        LiveEffect le = effectFrom(e.toObject(), &eok);
        if (eok) o.effects << le;
    }
    if (o.type == ObjType::Group) {
        for (const auto &cv : j["children"].toArray()) {
            VObject c;
            if (!objFrom(cv.toObject(), files, c, err)) return false;
            o.children << c;
        }
        if (j["mask"].isObject()) {
            const QJsonObject mj = j["mask"].toObject();
            VObject m;
            if (!objFrom(mj["object"].toObject(), files, m, err)) return false;
            o.maskObj = {m};
            o.maskMode = mj["mode"].toString() == QLatin1String("grayscale") ? VObject::MaskGrayscale : VObject::MaskOutline;
            o.maskEnabled = mj["enabled"].toBool(true);
            o.maskLinked = mj["linked"].toBool(true);
        }
    }
    return true;
}

QJsonObject exportJson(const ExportOptions &e) {
    QJsonObject j{{"format", ImageIO::formatKey(e.format)}, {"quality", e.quality}, {"lossless", e.lossless},
            {"colors", e.colors}, {"palette", e.palette}, {"dither", e.dither}, {"ditherAmount", e.ditherAmount}, {"loss", e.loss}, {"animated", e.animated},
            {"netpbmAscii", e.netpbmAscii}, {"tiffCompression", e.tiffCompression}, {"tiff16", e.tiff16},
            {"alpha", e.alpha}, {"tgaRle", e.tgaRle}, {"tgaTopLeft", e.tgaTopLeft}, {"matte", col(e.matte)}};
    // v0.2.5 options: written only when they differ from the defaults, so older files stay byte-identical
    const ExportOptions d;
    auto put = [&j](const char *k, const QJsonValue &v) { j.insert(QLatin1String(k), v); };
    if (e.webpEffort != d.webpEffort) put("webpEffort", e.webpEffort);
    if (e.webpAlphaQuality != d.webpAlphaQuality) put("webpAlphaQuality", e.webpAlphaQuality);
    if (e.webpSharpYuv != d.webpSharpYuv) put("webpSharpYuv", e.webpSharpYuv);
    if (e.webpExact != d.webpExact) put("webpExact", e.webpExact);
    if (e.avifSpeed != d.avifSpeed) put("avifSpeed", e.avifSpeed);
    if (e.avifChroma != d.avifChroma) put("avifChroma", e.avifChroma);
    if (e.avifDepth != d.avifDepth) put("avifDepth", e.avifDepth);
    if (e.avifAlphaQuality != d.avifAlphaQuality) put("avifAlphaQuality", e.avifAlphaQuality);
    if (e.jxlEffort != d.jxlEffort) put("jxlEffort", e.jxlEffort);
    if (e.heifPreset != d.heifPreset) put("heifPreset", e.heifPreset);
    if (e.heifChroma != d.heifChroma) put("heifChroma", e.heifChroma);
    if (e.ktxZstdLevel != d.ktxZstdLevel) put("ktxZstdLevel", e.ktxZstdLevel);
    if (e.tinyDither != d.tinyDither) put("tinyDither", e.tinyDither);
    if (e.pngType != d.pngType) put("pngType", e.pngType);
    if (e.png16 != d.png16) put("png16", e.png16);
    if (e.pngInterlace != d.pngInterlace) put("pngInterlace", e.pngInterlace);
    if (e.pngCompression != d.pngCompression) put("pngCompression", e.pngCompression);
    if (e.pngTransparency != d.pngTransparency) put("pngTransparency", e.pngTransparency);
    if (e.tinyPng != d.tinyPng) put("tinyPng", e.tinyPng);
    if (e.tinyQualityMin != d.tinyQualityMin) put("tinyQualityMin", e.tinyQualityMin);
    if (e.tinyQualityMax != d.tinyQualityMax) put("tinyQualityMax", e.tinyQualityMax);
    if (e.tinySpeed != d.tinySpeed) put("tinySpeed", e.tinySpeed);
    if (!e.customPalette.isEmpty()) {
        QJsonArray a;
        for (QRgb c : e.customPalette) a << (qAlpha(c) == 255 ? QColor(c).name() : QColor::fromRgba(c).name(QColor::HexArgb));
        put("customPalette", a);
    }
    if (e.pngEditable != d.pngEditable) put("pngEditable", e.pngEditable);
    if (e.bmpBits != d.bmpBits) put("bmpBits", e.bmpBits);
    if (e.bmpRle != d.bmpRle) put("bmpRle", e.bmpRle);
    if (e.icoSizes != d.icoSizes) put("icoSizes", e.icoSizes);
    if (e.icnsSizes != d.icnsSizes) put("icnsSizes", e.icnsSizes);
    if (e.curHotX != d.curHotX) put("curHotX", e.curHotX);
    if (e.curHotY != d.curHotY) put("curHotY", e.curHotY);
    if (e.ddsFormat != d.ddsFormat) put("ddsFormat", e.ddsFormat);
    if (e.mipmaps != d.mipmaps) put("mipmaps", e.mipmaps);
    if (e.ktxZstd != d.ktxZstd) put("ktxZstd", e.ktxZstd);
    if (e.exrCompression != d.exrCompression) put("exrCompression", e.exrCompression);
    if (e.exrHalf != d.exrHalf) put("exrHalf", e.exrHalf);
    if (e.pcx8 != d.pcx8) put("pcx8", e.pcx8);
    if (e.jpegEncoder != d.jpegEncoder) put("jpegEncoder", e.jpegEncoder);
    if (e.jpegProgressive != d.jpegProgressive) put("jpegProgressive", e.jpegProgressive);
    if (e.jpegOptimize != d.jpegOptimize) put("jpegOptimize", e.jpegOptimize);
    if (e.jpegSubsampling != d.jpegSubsampling) put("jpegSubsampling", e.jpegSubsampling);
    if (e.jpegSmoothing != d.jpegSmoothing) put("jpegSmoothing", e.jpegSmoothing);
    if (e.jpegSharpenEdges != d.jpegSharpenEdges) put("jpegSharpenEdges", e.jpegSharpenEdges);
    if (e.jpegStripMeta != d.jpegStripMeta) put("jpegStripMeta", e.jpegStripMeta);
    if (e.jpegKeepIcc != d.jpegKeepIcc) put("jpegKeepIcc", e.jpegKeepIcc);
    if (e.chPixel != d.chPixel) put("chPixel", e.chPixel);
    if (e.chElement != d.chElement) put("chElement", e.chElement);
    if (e.chHex != d.chHex) put("chHex", e.chHex);
    if (e.chPerLine != d.chPerLine) put("chPerLine", e.chPerLine);
    if (e.chConst != d.chConst) put("chConst", e.chConst);
    if (e.chProgmem != d.chProgmem) put("chProgmem", e.chProgmem);
    if (e.chAligned != d.chAligned) put("chAligned", e.chAligned);
    if (e.chSection != d.chSection) put("chSection", e.chSection);
    if (e.chSectionName != d.chSectionName) put("chSectionName", e.chSectionName);
    if (e.chStdint != d.chStdint) put("chStdint", e.chStdint);
    if (e.chDefines != d.chDefines) put("chDefines", e.chDefines);
    if (e.chFrameLayout != d.chFrameLayout) put("chFrameLayout", e.chFrameLayout);
    if (e.chDelays != d.chDelays) put("chDelays", e.chDelays);
    if (e.chRle != d.chRle) put("chRle", e.chRle);
    if (e.chStyle != d.chStyle) put("chStyle", e.chStyle);
    if (e.chRawBin != d.chRawBin) put("chRawBin", e.chRawBin);
    if (e.chLsbFirst != d.chLsbFirst) put("chLsbFirst", e.chLsbFirst);
    if (e.chInvert != d.chInvert) put("chInvert", e.chInvert);
    if (e.chName != d.chName) put("chName", e.chName);
    if (e.chGimpMacros != d.chGimpMacros) put("chGimpMacros", e.chGimpMacros);
    if (e.chGimpGlib != d.chGimpGlib) put("chGimpGlib", e.chGimpGlib);
    if (e.chGimpComment != d.chGimpComment) put("chGimpComment", e.chGimpComment);
    if (e.chGimpCommentText != d.chGimpCommentText) put("chGimpCommentText", e.chGimpCommentText);
    if (e.chGimpOpacity != d.chGimpOpacity) put("chGimpOpacity", e.chGimpOpacity);
    // the default of chStrip is the remembered preference, so write it whenever it matters (C header) or is off
    if (e.format == Fmt::CHeader || !e.chStrip) put("chStrip", e.chStrip);
    if (e.chDither != d.chDither) put("chDither", e.chDither);
    if (e.scalePct != d.scalePct) put("scalePct", e.scalePct);
    if (e.scaleResample != d.scaleResample) put("scaleResample", pixelscale::resampleKey(e.scaleResample));
    return j;
}
ExportOptions exportFrom(const QJsonObject &j) {
    ExportOptions e;
    if (j.isEmpty()) return e;
    Fmt f = ImageIO::formatFromKey(j["format"].toString());
    if (f != Fmt::Unknown) e.format = f;
    e.quality = j["quality"].toInt(e.quality); e.lossless = j["lossless"].toBool();
    e.colors = j["colors"].toInt(256); e.palette = j["palette"].toInt(); e.dither = j["dither"].toBool(true); e.ditherAmount = qBound(0, j["ditherAmount"].toInt(100), 100); e.loss = qBound(0, j["loss"].toInt(0), 100);
    e.animated = j["animated"].toBool(); e.netpbmAscii = j["netpbmAscii"].toBool();
    e.tiffCompression = qBound(0, j["tiffCompression"].toInt(e.tiffCompression), 3); e.tiff16 = j["tiff16"].toBool(e.tiff16);
    e.alpha = j["alpha"].toBool(e.alpha); e.tgaRle = j["tgaRle"].toBool(e.tgaRle); e.tgaTopLeft = j["tgaTopLeft"].toBool(e.tgaTopLeft);
    e.matte = toCol(j["matte"], Qt::white);
    auto gi = [&j](const char *k, int def, int lo, int hi) { return qBound(lo, j[QLatin1String(k)].toInt(def), hi); };
    auto gb = [&j](const char *k, bool def) { return j[QLatin1String(k)].toBool(def); };
    e.webpEffort = gi("webpEffort", e.webpEffort, 0, 6);
    e.webpAlphaQuality = gi("webpAlphaQuality", e.webpAlphaQuality, 0, 100);
    e.webpSharpYuv = gb("webpSharpYuv", e.webpSharpYuv);
    e.webpExact = gb("webpExact", e.webpExact);
    e.avifSpeed = gi("avifSpeed", e.avifSpeed, 0, 10);
    e.avifChroma = gi("avifChroma", e.avifChroma, 0, 2);
    e.avifDepth = gi("avifDepth", e.avifDepth, 8, 12);
    if (e.avifDepth != 8 && e.avifDepth != 10 && e.avifDepth != 12) e.avifDepth = 8;
    e.avifAlphaQuality = gi("avifAlphaQuality", e.avifAlphaQuality, -1, 100);
    e.jxlEffort = gi("jxlEffort", e.jxlEffort, 1, 9);
    e.heifPreset = j["heifPreset"].toString(e.heifPreset);
    e.heifChroma = gi("heifChroma", e.heifChroma, -1, 2);
    e.ktxZstdLevel = gi("ktxZstdLevel", e.ktxZstdLevel, 1, 22);
    e.tinyDither = gi("tinyDither", e.tinyDither, 0, 100);
    e.pngType = gi("pngType", e.pngType, 0, 3); e.png16 = gb("png16", e.png16); e.pngInterlace = gb("pngInterlace", e.pngInterlace);
    e.pngCompression = gi("pngCompression", e.pngCompression, 0, 9); e.pngTransparency = gi("pngTransparency", e.pngTransparency, 0, 2);
    e.tinyPng = gb("tinyPng", e.tinyPng); e.tinyQualityMin = gi("tinyQualityMin", e.tinyQualityMin, 0, 100);
    e.tinyQualityMax = gi("tinyQualityMax", e.tinyQualityMax, 0, 100); e.tinySpeed = gi("tinySpeed", e.tinySpeed, 1, 10);
    for (const auto &c : j["customPalette"].toArray()) e.customPalette << QColor(c.toString()).rgba();
    e.pngEditable = gb("pngEditable", e.pngEditable);
    e.bmpBits = gi("bmpBits", e.bmpBits, 1, 32); e.bmpRle = gb("bmpRle", e.bmpRle);
    e.icoSizes = gi("icoSizes", e.icoSizes, 1, 0x1FF); e.icnsSizes = gi("icnsSizes", e.icnsSizes, 1, 0x1FF);
    e.curHotX = gi("curHotX", e.curHotX, 0, 1 << 20); e.curHotY = gi("curHotY", e.curHotY, 0, 1 << 20);
    e.ddsFormat = gi("ddsFormat", e.ddsFormat, 0, 2); e.mipmaps = gb("mipmaps", e.mipmaps); e.ktxZstd = gb("ktxZstd", e.ktxZstd);
    e.exrCompression = gi("exrCompression", e.exrCompression, 0, 7); e.exrHalf = gb("exrHalf", e.exrHalf); e.pcx8 = gb("pcx8", e.pcx8);
    e.jpegEncoder = gi("jpegEncoder", e.jpegEncoder, 0, 2); e.jpegProgressive = gb("jpegProgressive", e.jpegProgressive);
    e.jpegOptimize = gb("jpegOptimize", e.jpegOptimize); e.jpegSubsampling = gi("jpegSubsampling", e.jpegSubsampling, 0, 2);
    e.jpegSmoothing = gi("jpegSmoothing", e.jpegSmoothing, 0, 8); e.jpegSharpenEdges = gb("jpegSharpenEdges", e.jpegSharpenEdges);
    e.jpegStripMeta = gb("jpegStripMeta", e.jpegStripMeta); e.jpegKeepIcc = gb("jpegKeepIcc", e.jpegKeepIcc);
    e.chPixel = gi("chPixel", e.chPixel, 0, 13); e.chElement = gi("chElement", e.chElement, 0, 1); e.chHex = gb("chHex", e.chHex);
    e.chPerLine = gi("chPerLine", e.chPerLine, 1, 256); e.chConst = gb("chConst", e.chConst); e.chProgmem = gb("chProgmem", e.chProgmem);
    e.chAligned = gb("chAligned", e.chAligned); e.chSection = gb("chSection", e.chSection);
    e.chSectionName = j["chSectionName"].toString(e.chSectionName); e.chStdint = gb("chStdint", e.chStdint);
    e.chDefines = gb("chDefines", e.chDefines); e.chFrameLayout = gi("chFrameLayout", e.chFrameLayout, 0, 1); e.chDelays = gb("chDelays", e.chDelays);
    e.chRle = gb("chRle", e.chRle); e.chStyle = gi("chStyle", e.chStyle, 0, 3); e.chRawBin = gb("chRawBin", e.chRawBin);
    e.chLsbFirst = gb("chLsbFirst", e.chLsbFirst); e.chInvert = gb("chInvert", e.chInvert); e.chName = j["chName"].toString();
    e.chGimpMacros = gb("chGimpMacros", e.chGimpMacros); e.chGimpGlib = gb("chGimpGlib", e.chGimpGlib);
    e.chGimpComment = gb("chGimpComment", e.chGimpComment);
    if (j.contains("chGimpCommentText")) e.chGimpCommentText = j["chGimpCommentText"].toString();
    e.chGimpOpacity = gi("chGimpOpacity", e.chGimpOpacity, 0, 100); e.chStrip = gb("chStrip", e.chStrip);
    e.scalePct = gi("scalePct", e.scalePct, 1, 1600);
    if (j.contains("scaleResample")) e.scaleResample = pixelscale::resampleFromKey(j["scaleResample"].toString(), -1);
    e.chDither = gi("chDither", e.chDither, 0, 7);
    return e;
}
}

QJsonObject effectToJson(const LiveEffect &e) { return effectJson(e); }
LiveEffect effectFromJson(const QJsonObject &j, bool *ok) { return effectFrom(j, ok); }
QJsonObject objectToJson(const VObject &o) {
    QVector<zip::Entry> files; // bitmaps are not supported here (their pixels would be dropped)
    return objJson(o, files);
}
bool objectFromJson(const QJsonObject &j, VObject &o) { return objFrom(j, {}, o, nullptr); }

static QJsonArray frameLayersJson(const DocState &st, int frame, QVector<zip::Entry> &none)
{
    QJsonArray layers;
    if (frame >= 0 && frame < st.frames.size())
        for (int li = 0; li < st.frames[frame].layers.size(); ++li) {
            QJsonArray objs;
            for (const VObject &o : st.frames[frame].layers[li]) objs.append(objJson(o, none, QString(), true));
            const bool vis = li < st.layers.size() && st.layers[li].visible, web = li < st.layers.size() && st.layers[li].web;
            layers.append(QJsonObject{{"visible", vis}, {"web", web}, {"objects", objs}});
        }
    return layers;
}

static QJsonArray libraryJson(const DocState &st, QVector<zip::Entry> &none, bool withNameKind)
{
    QJsonArray symbols;
    for (const Symbol &sym : st.symbols) {
        QJsonArray frames;
        for (const auto &fr : sym.frames) {
            QJsonArray objs;
            for (const VObject &o : fr) objs.append(objJson(o, none, QString(), true));
            frames.append(objs);
        }
        QJsonObject o{{"id", QString::number(sym.id)}, {"frames", frames}};
        if (withNameKind) {
            o.insert(QStringLiteral("name"), sym.name);
            o.insert(QStringLiteral("kind"), int(sym.kind));
        }
        symbols.append(o);
    }
    return symbols;
}

QByteArray contentKey(const DocState &st, int frame) {
    QVector<zip::Entry> none;
    const QJsonObject j{{"w", st.width}, {"h", st.height}, {"canvas", col(st.canvasColor)},
                        {"layers", frameLayersJson(st, frame, none)},
                        {"symbols", libraryJson(st, none, false)}};
    return QCryptographicHash::hash(QJsonDocument(j).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
}

QByteArray libraryKey(const DocState &st) {
    QVector<zip::Entry> none;
    return QCryptographicHash::hash(QJsonDocument(libraryJson(st, none, true)).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha1);
}

QByteArray frameLayerKey(const DocState &st, int frame) {
    QVector<zip::Entry> none;
    return QCryptographicHash::hash(QJsonDocument(frameLayersJson(st, frame, none)).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha1);
}

QByteArray objectContentKey(const VObject &o) {
    QVector<zip::Entry> none;
    QJsonObject j = objJson(o, none, QString(), true);
    // renderObject draws the base + effects; opacity / blend are applied by drawObject at blit time
    j.remove(QStringLiteral("id"));
    j.remove(QStringLiteral("name"));
    j.remove(QStringLiteral("opacity"));
    j.remove(QStringLiteral("blend"));
    j.remove(QStringLiteral("visible"));
    j.remove(QStringLiteral("locked"));
    return QCryptographicHash::hash(QJsonDocument(j).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
}

QByteArray serialize(const Document &doc) {
    QVector<zip::Entry> files;
    QVector<PendingPng> pending;
    struct PendingScope { QVector<PendingPng> *old; explicit PendingScope(QVector<PendingPng> *p) : old(t_pending) { t_pending = p; } ~PendingScope() { t_pending = old; } }
        pendingScope(&pending);
    // the flattened preview (for file managers) renders on the pool while the JSON is built and the images
    // encode; an unchanged frame 0 (same content key) reuses the last preview
    static QMutex thumbMutex;
    static QByteArray lastThumbKey, lastThumb;
    const QByteArray thumbKey = contentKey(doc.s, 0);
    QByteArray cachedThumb;
    {
        QMutexLocker lock(&thumbMutex);
        if (thumbKey == lastThumbKey) cachedThumb = lastThumb;
    }
    QFuture<QByteArray> thumbFuture;
    if (cachedThumb.isEmpty()) {
        // Stamp a copy on the GUI thread so the worker never writes live objects, and so the
        // preview can reuse the (mutexed) object visual cache of already-painted bitmaps.
        DocState thumbState = doc.s;
        Document::stampUnsetBitmaps(thumbState);
        thumbFuture = QtConcurrent::run([thumbState, thumbKey] {
            QByteArray thumb;
            QBuffer b(&thumb);
            b.open(QIODevice::WriteOnly);
            render::renderFrame(thumbState, 0, true).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&b, "PNG");
            QMutexLocker lock(&thumbMutex);
            lastThumbKey = thumbKey;
            lastThumb = thumb;
            return thumb;
        });
    }
    const DocState &s = doc.s;
    QJsonArray layers;
    for (const auto &l : s.layers)
        {
        QJsonObject lj{{"id", QString::number(l.id)}, {"name", l.name}, {"visible", l.visible}, {"locked", l.locked}};
        if (l.shared) lj["shared"] = true;
        if (l.web) lj["web"] = true;
        layers.append(lj);
    }
    QJsonArray frames;
    for (const auto &f : s.frames) {
        QJsonArray fl;
        for (const auto &objs : f.layers) {
            QJsonArray oa;
            for (const auto &o : objs) oa.append(objJson(o, files));
            fl.append(oa);
        }
        frames.append(QJsonObject{{"name", f.name}, {"delay", f.delayMs}, {"layers", fl}});
    }
    QJsonArray symbols;
    for (const Symbol &sym : s.symbols) {
        QJsonArray sf;
        for (int fi = 0; fi < sym.frames.size(); ++fi) {
            QJsonArray oa;
            for (const auto &o : sym.frames[fi]) oa.append(objJson(o, files, QStringLiteral("sym%1_%2_").arg(sym.id).arg(fi)));
            sf.append(oa);
        }
        symbols.append(QJsonObject{{"id", QString::number(sym.id)}, {"name", sym.name}, {"kind", symbolKindKey(sym.kind)}, {"frames", sf}});
    }
    QJsonObject root{{"format", "libreworks-document"}, {"version", 7}, {"generator", QStringLiteral(LW_PRODUCT_NAME) + QLatin1Char(' ') + QStringLiteral(LW_VERSION)},
                     {"width", s.width}, {"height", s.height}, {"canvasColor", col(s.canvasColor)}, {"loops", s.loops},
                     {"currentFrame", doc.curFrame}, {"currentLayer", doc.curLayer},
                     {"layers", layers}, {"frames", frames}, {"export", exportJson(doc.exportOptions)}};
    if (!symbols.isEmpty()) root["symbols"] = symbols;
    if (s.dpi != 72) root["resolution"] = s.dpi;   // pixels/inch (v0.3.1)
    root["grid"] = QJsonObject{{"x", doc.grid.x}, {"y", doc.grid.y}, {"color", col(doc.grid.color)}, {"show", doc.grid.show}, {"snap", doc.grid.snap}};
    {
        QJsonArray gv, gh;
        for (double x : doc.guides.v) gv.append(x);
        for (double y : doc.guides.h) gh.append(y);
        root["guides"] = QJsonObject{{"vertical", gv}, {"horizontal", gh}, {"color", col(doc.guides.color)},
                                     {"show", doc.guides.show}, {"lock", doc.guides.lock}, {"snap", doc.guides.snap}};
    }
    if (!doc.toolEnv.isEmpty()) root["toolEnvironment"] = doc.toolEnv;
    if (doc.paneOptions.size() == Document::kPanes) {
        QJsonArray pa;
        for (int i = 0; i < Document::kPanes; ++i) pa.append(exportJson(doc.paneOption(i)));
        root["previewPanes"] = pa;
        root["selectedPane"] = doc.selectedPane;
    }
    if (!doc.savedSelections.isEmpty()) {   // Save Bitmap Selection: named 8-bit masks
        QJsonArray sa;
        for (int i = 0; i < doc.savedSelections.size(); ++i) {
            const auto &ss = doc.savedSelections[i];
            QByteArray png;
            { QBuffer b(&png); b.open(QIODevice::WriteOnly); QImage g = ss.mask.convertToFormat(QImage::Format_Grayscale8);
              for (int y = 0; y < g.height(); ++y) memcpy(g.scanLine(y), ss.mask.constScanLine(y), size_t(g.width()));
              g.save(&b, "PNG"); }
            const QString name = QStringLiteral("selections/selection%1.png").arg(i);
            files.append({name, png, false});
            sa.append(QJsonObject{{"name", ss.name}, {"x", ss.origin.x()}, {"y", ss.origin.y()}, {"mask", name}});
        }
        root["savedSelections"] = sa;
    }
    QVector<zip::Entry> all;
    all.append({QStringLiteral("mimetype"), QByteArrayLiteral("application/x-libreworks"), false});
    all.append({QStringLiteral("document.json"), QJsonDocument(root).toJson(QJsonDocument::Indented), true});
    zip::Entry *entries = files.data();
    QtConcurrent::blockingMap(pending, [entries](const PendingPng &p) { entries[p.index].data = pngBytes(p.image); });
    all.append({QStringLiteral("preview.png"), cachedThumb.isEmpty() ? thumbFuture.result() : cachedThumb, false});
    all += files;
    return zip::write(all);
}

bool save(const Document &doc, const QString &path, QString *err) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(serialize(doc));
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    return true;
}

Document *deserialize(const QByteArray &data, QString *err) {
    QMap<QString, QByteArray> files;
    if (!zip::read(data, files, err)) return nullptr;
    QJsonParseError pe;
    QJsonDocument jd = QJsonDocument::fromJson(files.value(QStringLiteral("document.json")), &pe);
    QJsonObject root = jd.object();
    if (pe.error != QJsonParseError::NoError || root["format"].toString() != QLatin1String("libreworks-document")) {
        if (err) *err = QStringLiteral("not a %1 document").arg(QStringLiteral(LW_PRODUCT_NAME));
        return nullptr;
    }
    // version 7 = 0.3.20 (Edit Stroke params); 6 = 0.3.16 (per-bitmap interpolation); 5 = tool env; 4 = handles; 3 = groups/masks
    if (root["version"].toInt() > 7) { if (err) *err = QStringLiteral("document was written by a newer %1").arg(QStringLiteral(LW_PRODUCT_NAME)); return nullptr; }
    struct VersionScope { int old; explicit VersionScope(int v) : old(s_readVersion) { s_readVersion = v; } ~VersionScope() { s_readVersion = old; } }
        versionScope(qMax(1, root["version"].toInt(1)));
    auto *doc = new Document;
    DocState &s = doc->s;
    s.width = qMax(1, root["width"].toInt(500));
    s.height = qMax(1, root["height"].toInt(500));
    s.canvasColor = toCol(root["canvasColor"], Qt::white);
    s.loops = root["loops"].toInt();
    s.dpi = qBound(1.0, root["resolution"].toDouble(72), 100000.0);
    if (root["grid"].isObject()) {   // older files: the Preferences default (set by the constructor)
        const QJsonObject g = root["grid"].toObject();
        doc->grid.x = qBound(1, g["x"].toInt(doc->grid.x), 10000);
        doc->grid.y = qBound(1, g["y"].toInt(doc->grid.y), 10000);
        doc->grid.color = toCol(g["color"], doc->grid.color);
        doc->grid.show = g["show"].toBool();
        doc->grid.snap = g["snap"].toBool();
    }
    for (const auto &sv : root["savedSelections"].toArray()) {
        const QJsonObject so = sv.toObject();
        QImage g;
        g.loadFromData(files.value(so["mask"].toString()), "PNG");
        if (g.isNull()) continue;
        g = g.convertToFormat(QImage::Format_Grayscale8);
        QImage m(g.size(), QImage::Format_Alpha8);
        for (int y = 0; y < g.height(); ++y) memcpy(m.scanLine(y), g.constScanLine(y), size_t(g.width()));
        doc->savedSelections.append({so["name"].toString(), m, QPoint(so["x"].toInt(), so["y"].toInt())});
    }
    if (root["guides"].isObject()) {
        const QJsonObject g = root["guides"].toObject();
        for (const auto &x : g["vertical"].toArray()) doc->guides.v << x.toDouble();
        for (const auto &y : g["horizontal"].toArray()) doc->guides.h << y.toDouble();
        doc->guides.color = toCol(g["color"], doc->guides.color);
        doc->guides.show = g["show"].toBool(true);
        doc->guides.lock = g["lock"].toBool();
        doc->guides.snap = g["snap"].toBool(true);
    }
    if (root["toolEnvironment"].isObject()) doc->toolEnv = root["toolEnvironment"].toObject();
    for (const auto &lv : root["layers"].toArray()) {
        QJsonObject l = lv.toObject();
        LayerInfo li{l["id"].toString().toULongLong(), l["name"].toString(), l["visible"].toBool(true), l["locked"].toBool()};
        li.web = l["web"].toBool();
        li.shared = li.web || l["shared"].toBool();
        s.layers.append(li);
    }
    for (const auto &fv : root["frames"].toArray()) {
        QJsonObject fj = fv.toObject();
        Frame f;
        f.name = fj["name"].toString();
        f.delayMs = fj["delay"].toInt(100);
        for (const auto &lv : fj["layers"].toArray()) {
            QVector<VObject> objs;
            for (const auto &ov : lv.toArray()) {
                VObject o;
                if (!objFrom(ov.toObject(), files, o, err)) { delete doc; return nullptr; }
                objs.append(o);
            }
            f.layers.append(objs);
        }
        f.layers.resize(s.layers.size());
        s.frames.append(f);
    }
    for (const auto &sv : root["symbols"].toArray()) {
        QJsonObject sj = sv.toObject();
        Symbol sym;
        sym.id = sj["id"].toString().toULongLong();
        sym.name = sj["name"].toString();
        sym.kind = symbolKindFromKey(sj["kind"].toString());
        for (const auto &fv : sj["frames"].toArray()) {
            QVector<VObject> objs;
            for (const auto &ov : fv.toArray()) {
                VObject o;
                if (!objFrom(ov.toObject(), files, o, err)) { delete doc; return nullptr; }
                objs.append(o);
            }
            sym.frames.append(objs);
        }
        if (sym.frames.isEmpty()) sym.frames.resize(1);
        s.symbols.append(sym);
    }
    doc->exportOptions = exportFrom(root["export"].toObject());
    if (const QJsonArray pa = root["previewPanes"].toArray(); pa.size() == Document::kPanes) {
        doc->paneOptions.clear();
        for (const auto &v : pa) doc->paneOptions.append(exportFrom(v.toObject()));
        doc->selectedPane = qBound(0, root["selectedPane"].toInt(), Document::kPanes - 1);
        doc->exportOptions = doc->paneOptions[doc->selectedPane];
    }
    doc->curFrame = root["currentFrame"].toInt();
    doc->curLayer = root["currentLayer"].toInt();
    doc->resetHistory(QStringLiteral("Open"));
    return doc;
}

Document *load(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return nullptr; }
    Document *d = deserialize(f.readAll(), err);
    if (d) d->filePath = path;
    return d;
}
QJsonObject exportOptionsToJson(const ExportOptions &e) { return exportJson(e); }
ExportOptions exportOptionsFromJson(const QJsonObject &j) { return exportFrom(j); }
}
