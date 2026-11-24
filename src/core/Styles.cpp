#include "Brand.h"
#include <utility>
#include "Styles.h"
#include "Fills.h"
#include "Render.h"
#include "io/LwdFormat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>

namespace {
// The attribute groups. Kept in one place so new fill/stroke attributes only need adding here.
void copyFill(const VObject &from, VObject &to) {
    to.fillOn = from.fillOn; to.fill = from.fill; to.fillKind = from.fillKind; to.gradient = from.gradient;
    to.gradP0 = from.gradP0; to.gradP1 = from.gradP1; to.gradP2 = from.gradP2; to.patternXform = from.patternXform;
    to.pattern = from.pattern; to.patternImage = from.patternImage; to.texture = from.texture; to.textureProc = from.textureProc; to.patternProc = from.patternProc;
    to.textureAmount = from.textureAmount; to.feather = from.feather;
    if (to.isVector()) to.antialias = from.antialias;
}
void copyStroke(const VObject &from, VObject &to) { to.strokeOn = from.strokeOn; to.stroke = from.stroke; to.strokeWidth = from.strokeWidth; to.strokeKind = from.strokeKind; to.strokeAlign = from.strokeAlign;
    to.strokeParams = from.strokeParams; to.strokeCustom = from.strokeCustom;
    to.strokeTexture = from.strokeTexture; to.strokeTextureProc = from.strokeTextureProc; to.strokeTextureAmount = from.strokeTextureAmount; }
void copyText(const VObject &from, VObject &to) {
    to.fontFamily = from.fontFamily; to.fontSize = from.fontSize; to.bold = from.bold; to.italic = from.italic; to.textAA = from.textAA;
    to.underline = from.underline; to.kerning = from.kerning; to.leading = from.leading; to.hScale = from.hScale; to.baselineShift = from.baselineShift;
}
bool sameFill(const VObject &a, const VObject &b) {
    return a.fillOn == b.fillOn && a.fill == b.fill && a.fillKind == b.fillKind && a.gradient == b.gradient && a.gradP0 == b.gradP0
        && a.gradP1 == b.gradP1 && a.gradP2 == b.gradP2 && a.patternXform == b.patternXform && a.pattern == b.pattern && a.texture == b.texture && a.textureProc == b.textureProc && a.patternProc == b.patternProc
        && a.textureAmount == b.textureAmount && a.feather == b.feather;
}
bool sameStroke(const VObject &a, const VObject &b) { return a.strokeOn == b.strokeOn && a.stroke == b.stroke && a.strokeWidth == b.strokeWidth && a.strokeKind == b.strokeKind && a.strokeAlign == b.strokeAlign
    && a.strokeCustom == b.strokeCustom && a.strokeParams == b.strokeParams
    && a.strokeTexture == b.strokeTexture && a.strokeTextureProc == b.strokeTextureProc && a.strokeTextureAmount == b.strokeTextureAmount; }
bool sameText(const VObject &a, const VObject &b) {
    return a.fontFamily == b.fontFamily && a.fontSize == b.fontSize && a.bold == b.bold && a.italic == b.italic && a.textAA == b.textAA
        && a.underline == b.underline && a.kerning == b.kerning && a.leading == b.leading && a.hScale == b.hScale && a.baselineShift == b.baselineShift;
}
bool sameEffects(const QVector<LiveEffect> &a, const QVector<LiveEffect> &b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (lwd::effectToJson(a[i]) != lwd::effectToJson(b[i])) return false;
    return true;
}
}

Style Style::fromObject(const VObject &o, const QString &name) {
    Style s;
    s.name = name;
    s.attrs.type = o.type == ObjType::Text ? ObjType::Text : ObjType::Rect;
    s.attrs.rect = QRectF(0, 0, 10, 10);
    copyFill(o, s.attrs);
    copyStroke(o, s.attrs);
    copyText(o, s.attrs);
    s.attrs.effects = o.effects;
    s.hasText = o.type == ObjType::Text;
    if (o.type == ObjType::Bitmap) s.hasFill = s.hasStroke = false;
    return s;
}

bool Style::applyTo(VObject &o) const {
    if (o.isWeb()) return false;
    bool changed = false;
    const bool vector = o.isVector();
    if (hasFill && vector && !sameFill(o, attrs)) { copyFill(attrs, o); changed = true; }
    if (hasStroke && vector && !sameStroke(o, attrs)) { copyStroke(attrs, o); changed = true; }
    if (hasEffects && !sameEffects(o.effects, attrs.effects)) { o.effects = attrs.effects; changed = true; }
    if (hasText && o.type == ObjType::Text && !sameText(o, attrs)) { copyText(attrs, o); o.fitTextBox(); changed = true; }
    return changed;
}

namespace styles {

QVector<Style> builtins() {
    QVector<Style> v;
    auto add = [&v](const QString &name, QColor fill, bool strokeOn, QColor stroke, double sw, QVector<LiveEffect> fx) {
        Style s;
        s.name = name;
        s.builtin = true;
        s.attrs.rect = QRectF(0, 0, 10, 10);
        s.attrs.fillOn = fill.isValid();
        if (fill.isValid()) s.attrs.fill = fill;
        s.attrs.strokeOn = strokeOn;
        s.attrs.stroke = stroke;
        s.attrs.strokeWidth = sw;
        s.attrs.effects = std::move(fx);
        v << s;
        return &v.last();
    };
    auto fx = [](EffectType t, auto setup) { LiveEffect e = LiveEffect::make(t); setup(e); return e; };
    auto none = [](LiveEffect &) {};
    add(QStringLiteral("Plain Blue"), QColor(0x33, 0x66, 0xcc), false, Qt::black, 1, {});
    add(QStringLiteral("Soft Shadow"), QColor(0xf2, 0xf2, 0xf2), true, QColor(0x99, 0x99, 0x99), 1,
        {fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 4; e.softness = 6; e.opacity = 45; })});
    add(QStringLiteral("Raised Button"), QColor(0x99, 0x99, 0x99), false, Qt::black, 1,
        {fx(EffectType::InnerBevel, [](LiveEffect &e) { e.width = 5; })});
    add(QStringLiteral("Inset Panel"), QColor(0xcc, 0xcc, 0xcc), false, Qt::black, 1,
        {fx(EffectType::InnerBevel, [](LiveEffect &e) { e.preset = BevelPreset::Inset; e.width = 4; })});
    add(QStringLiteral("Aqua Gel"), QColor(0x2a, 0x8c, 0xe8), true, QColor(0x10, 0x50, 0xa0), 1,
        {fx(EffectType::InnerBevel, [](LiveEffect &e) { e.width = 10; e.softness = 6; e.opacity = 90; }),
         fx(EffectType::InnerGlow, [](LiveEffect &e) { e.color = QColor(0xcc, 0xee, 0xff); e.width = 3; }),
         fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 3; e.softness = 4; e.opacity = 40; e.color = QColor(0x10, 0x30, 0x60); })});
    add(QStringLiteral("Chrome"), QColor(0xb8, 0xbc, 0xc4), true, QColor(0x55, 0x55, 0x60), 1,
        {fx(EffectType::InnerBevel, [](LiveEffect &e) { e.preset = BevelPreset::Highlighted; e.width = 8; e.opacity = 100; }),
         fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 2; e.softness = 2; e.opacity = 50; })});
    add(QStringLiteral("Gold"), QColor(0xd4, 0xa0, 0x17), true, QColor(0x8a, 0x60, 0x00), 1,
        {fx(EffectType::InnerBevel, [](LiveEffect &e) { e.width = 6; e.opacity = 90; }),
         fx(EffectType::OuterGlow, [](LiveEffect &e) { e.color = QColor(0xff, 0xe0, 0x80); e.width = 3; e.opacity = 60; })});
    add(QStringLiteral("Neon Glow"), QColor(0x10, 0x10, 0x18), true, QColor(0x00, 0xff, 0xcc), 2,
        {fx(EffectType::OuterGlow, [](LiveEffect &e) { e.color = QColor(0x00, 0xff, 0xcc); e.width = 6; e.softness = 8; e.opacity = 85; })});
    add(QStringLiteral("Red Glow"), QColor(0xe0, 0x20, 0x20), false, Qt::black, 1,
        {fx(EffectType::OuterGlow, [](LiveEffect &e) { e.color = QColor(0xff, 0x50, 0x30); e.width = 5; e.softness = 6; })});
    add(QStringLiteral("Embossed Stone"), QColor(0x8a, 0x84, 0x78), false, Qt::black, 1,
        {fx(EffectType::Noise, [](LiveEffect &e) { e.amount = 20; }),
         fx(EffectType::RaisedEmboss, [](LiveEffect &e) { e.width = 4; })});
    add(QStringLiteral("Carved"), QColor(0xd8, 0xc8, 0xa8), false, Qt::black, 1,
        {fx(EffectType::InsetEmboss, [](LiveEffect &e) { e.width = 3; }), fx(EffectType::InnerShadow, [](LiveEffect &e) { e.distance = 2; e.opacity = 50; })});
    add(QStringLiteral("Outline Only"), QColor(), true, QColor(0x33, 0x33, 0x33), 2, {});
    add(QStringLiteral("Sunken Well"), QColor(0x55, 0x66, 0x77), false, Qt::black, 1,
        {fx(EffectType::InnerShadow, [](LiveEffect &e) { e.distance = 4; e.softness = 5; e.opacity = 80; })});
    add(QStringLiteral("Orange Pop"), QColor(0xff, 0x88, 0x00), true, QColor(0xff, 0xff, 0xff), 2,
        {fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 5; e.softness = 0; e.opacity = 100; e.color = QColor(0x40, 0x20, 0x00); })});
    add(QStringLiteral("Bevelled Frame"), QColor(0x66, 0x88, 0xaa), false, Qt::black, 1,
        {fx(EffectType::OuterBevel, [](LiveEffect &e) { e.width = 6; e.color = QColor(0x88, 0x88, 0x99); })});
    add(QStringLiteral("Soft Focus"), QColor(0x99, 0x66, 0xcc), false, Qt::black, 1, {fx(EffectType::GaussianBlur, [](LiveEffect &e) { e.softness = 2; })});
    Q_UNUSED(none);
    // gradient fills (handles are normalised, so they follow whatever object the style lands on)
    auto grad = [&](const QString &name, FillKind k, const QString &preset, bool vertical, QVector<LiveEffect> e) {
        Style *s = add(name, QColor(0x99, 0x99, 0x99), false, Qt::black, 1, std::move(e));
        fills::setFillKind(s->attrs, k);
        s->attrs.gradient = fills::gradientPreset(preset);
        if (vertical && k == FillKind::Linear) { s->attrs.gradP0 = {0.5, 0}; s->attrs.gradP1 = {0.5, 1}; s->attrs.gradP2 = {1, 0}; }
    };
    grad(QStringLiteral("Silver Gradient"), FillKind::Linear, QStringLiteral("Silver"), true,
         {fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 3; e.softness = 4; e.opacity = 40; })});
    grad(QStringLiteral("Copper Satin"), FillKind::Satin, QStringLiteral("Copper"), false,
         {fx(EffectType::InnerBevel, [](LiveEffect &e) { e.width = 4; e.opacity = 60; })});
    grad(QStringLiteral("Spectrum Cone"), FillKind::Cone, QStringLiteral("Spectrum"), false, {});
    grad(QStringLiteral("Cobalt Orb"), FillKind::Radial, QStringLiteral("Cobalt Blue"), false,
         {fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 4; e.softness = 5; e.opacity = 45; })});
    // text styles
    auto text = [&](const QString &name, QColor fill, QVector<LiveEffect> e, bool bold, bool italic, double size) {
        Style *s = add(name, fill, false, Qt::black, 1, std::move(e));
        s->hasText = true;
        s->attrs.type = ObjType::Text;
        s->attrs.bold = bold;
        s->attrs.italic = italic;
        s->attrs.fontSize = size;
    };
    text(QStringLiteral("Heading Shadow"), QColor(0x22, 0x22, 0x22), {fx(EffectType::DropShadow, [](LiveEffect &e) { e.distance = 2; e.softness = 2; e.opacity = 50; })}, true, false, 36);
    text(QStringLiteral("Glowing Text"), QColor(Qt::white), {fx(EffectType::OuterGlow, [](LiveEffect &e) { e.color = QColor(0x33, 0x99, 0xff); e.width = 4; e.softness = 5; })}, true, false, 28);
    text(QStringLiteral("Embossed Text"), QColor(0x99, 0x99, 0x99), {fx(EffectType::RaisedEmboss, [](LiveEffect &e) { e.width = 2; })}, true, false, 32);
    text(QStringLiteral("Italic Caption"), QColor(0x55, 0x55, 0x55), {}, false, true, 14);
    return v;
}

QString userFile() {
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return base + QLatin1Char('/') + brand::nameLower() + QStringLiteral("/styles.json");
}

QJsonObject toJson(const Style &s) {
    QJsonObject j{{"name", s.name}, {"fill", s.hasFill}, {"stroke", s.hasStroke}, {"effects", s.hasEffects}, {"text", s.hasText},
            {"attributes", lwd::objectToJson(s.attrs)}};
    if (s.builtin) j["builtin"] = true;
    return j;
}

bool fromJson(const QJsonObject &j, Style &s) {
    s.name = j["name"].toString();
    if (s.name.isEmpty()) return false;
    s.hasFill = j["fill"].toBool(true);
    s.hasStroke = j["stroke"].toBool(true);
    s.hasEffects = j["effects"].toBool(true);
    s.hasText = j["text"].toBool(false);
    s.builtin = j["builtin"].toBool(false);
    VObject a;
    if (!lwd::objectFromJson(j["attributes"].toObject(), a)) return false;
    if (a.type != ObjType::Text) a.type = ObjType::Rect;
    // text-only attributes are not written for rect prototypes
    const QJsonObject aj = j["attributes"].toObject();
    a.fontFamily = aj["font"].toString(a.fontFamily);
    a.fontSize = aj["fontSize"].toDouble(a.fontSize);
    a.bold = aj["bold"].toBool(a.bold);
    a.italic = aj["italic"].toBool(a.italic);
    s.attrs = a;
    return true;
}

QVector<Style> loadUser(QString *err) {
    QVector<Style> out;
    QFile f(userFile());
    if (!f.exists()) return out;
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return out; }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull()) { if (err) *err = pe.errorString(); return out; }
    for (const auto &v : doc.object()["styles"].toArray()) {
        Style s;
        if (fromJson(v.toObject(), s) && !s.builtin) out << s;
    }
    return out;
}

namespace {
QJsonDocument readJson(const QString &path, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return {}; }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull() && err) *err = pe.errorString();
    return doc;
}
bool writeJson(const QString &path, const QVector<Style> &list, bool library, QString *err) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { if (err) *err = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath()); return false; }
    QJsonArray a;
    for (const Style &s : list) a.append(toJson(s));
    QJsonObject root{{"format", "libreworks-styles"}, {"version", 2}, {"styles", a}};
    if (library) root["library"] = true;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(QJsonDocument(root).toJson());
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    return true;
}
}

QVector<Style> loadLibrary(QString *err) {
    if (QFile::exists(userFile())) {
        const QJsonDocument doc = readJson(userFile(), err);
        if (doc.object()["library"].toBool()) {
            QVector<Style> out;
            for (const auto &v : doc.object()["styles"].toArray()) {
                Style s;
                if (fromJson(v.toObject(), s)) out << s;
            }
            return out;
        }
    }
    return builtins() + loadUser(err);
}

bool saveLibrary(const QVector<Style> &all, QString *err) { return writeJson(userFile(), all, true, err); }

bool exportFile(const QString &path, const QVector<Style> &list, QString *err) {
    QVector<Style> l = list;
    for (Style &s : l) s.builtin = false;
    return writeJson(path, l, false, err);
}

QVector<Style> importFile(const QString &path, QString *err) {
    QVector<Style> out;
    const QJsonDocument doc = readJson(path, err);
    if (doc.isNull()) return out;
    if (doc.object()["format"].toString() != QLatin1String("libreworks-styles")) { if (err) *err = QStringLiteral("not a %1 styles file").arg(brand::name()); return out; }
    for (const auto &v : doc.object()["styles"].toArray()) {
        Style s;
        if (fromJson(v.toObject(), s)) { s.builtin = false; out << s; }
    }
    if (out.isEmpty() && err && err->isEmpty()) *err = QStringLiteral("no styles in the file");
    return out;
}

QString describe(const Style &s) {
    QStringList parts;
    if (s.hasFill) parts << QStringLiteral("Fill");
    if (s.hasStroke && s.attrs.strokeOn) parts << QStringLiteral("Stroke");
    if (s.hasEffects && !s.attrs.effects.isEmpty()) {
        QStringList fx;
        for (const LiveEffect &e : s.attrs.effects) fx << effectName(e.type);
        parts << QStringLiteral("Effects: ") + fx.join(QStringLiteral(", "));
    }
    if (s.hasText) parts << QStringLiteral("Text");
    return parts.isEmpty() ? QStringLiteral("No attributes") : parts.join(QStringLiteral("; "));
}

bool saveUser(const QVector<Style> &user, QString *err) {
    const QString path = userFile();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { if (err) *err = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath()); return false; }
    QJsonArray a;
    for (const Style &s : user) if (!s.builtin) a.append(toJson(s));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(QJsonDocument(QJsonObject{{"format", "libreworks-styles"}, {"version", 1}, {"styles", a}}).toJson());
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    return true;
}

QImage preview(const Style &s, int size) {
    VObject o;
    o.id = 1;
    if (s.hasText) {
        o.type = ObjType::Text;
        o.text = QStringLiteral("Aa");
        o.fillOn = true;
        o.fill = Qt::black;
        o.strokeOn = false;
        o.fontSize = size * 0.5;
        o.rect = QRectF(0, 0, 10, 10);
    } else {
        o.type = ObjType::Rect;
        o.roundness = 25;
        o.fill = QColor(0x99, 0x99, 0x99);
        o.strokeOn = false;
    }
    s.applyTo(o);
    if (s.hasText) { o.fontSize = size * 0.5; o.fitTextBox(); }
    const double pad = size * 0.2;
    if (s.hasText) o.rect.moveCenter(QPointF(size / 2.0, size / 2.0));
    else o.rect = QRectF(pad, pad, size - 2 * pad, size - 2 * pad);
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    render::drawObject(p, o, QRect(0, 0, size, size));
    return img;
}
}
