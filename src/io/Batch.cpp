#include "Brand.h"
#include "core/PixelScale.h"
#include "Batch.h"
#include "DocIO.h"
#include "ExportPresets.h"
#include "LwdFormat.h"
#include "core/Document.h"
#include "core/Effects.h"
#include "core/FindReplace.h"
#include "core/FxExtra.h"
#include "core/Filters.h"
#include "core/Transform.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

namespace batch {

static const char *kStepKeys[] = {"export", "scale", "replaceColor", "replaceText", "rename", "command", "filters", "convert"};

QStringList stepTypeNames() {
    return {QStringLiteral("Export (preset)"), QStringLiteral("Scale"), QStringLiteral("Find & Replace Color"), QStringLiteral("Find & Replace Text"),
            QStringLiteral("Rename"), QStringLiteral("Command"), QStringLiteral("Filters"), QStringLiteral("Convert Format")};
}
QString stepTypeKey(int t) { return t >= 0 && t < kStepTypes ? QString::fromLatin1(kStepKeys[t]) : QString(); }
int stepTypeFromKey(const QString &k) {
    for (int i = 0; i < kStepTypes; ++i) if (k.compare(QLatin1String(kStepKeys[i]), Qt::CaseInsensitive) == 0) return i;
    return -1;
}

QStringList commandKeys() {
    return {QStringLiteral("flip-h"), QStringLiteral("flip-v"), QStringLiteral("rotate-90"), QStringLiteral("rotate-180"), QStringLiteral("rotate-270"),
            QStringLiteral("trim"), QStringLiteral("fit-canvas"), QStringLiteral("flatten-layers"), QStringLiteral("grayscale"), QStringLiteral("invert"),
            QStringLiteral("auto-contrast")};
}
QStringList commandLabels() {
    return {QStringLiteral("Flip Canvas Horizontal"), QStringLiteral("Flip Canvas Vertical"), QStringLiteral("Rotate Canvas 90° CW"),
            QStringLiteral("Rotate Canvas 180°"), QStringLiteral("Rotate Canvas 90° CCW"), QStringLiteral("Trim Canvas"), QStringLiteral("Fit Canvas"),
            QStringLiteral("Flatten Layers"), QStringLiteral("Grayscale"), QStringLiteral("Invert"), QStringLiteral("Auto Contrast")};
}

static QString colorName(const QColor &c) { return c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb); }

static QString areaText(const Step &s) {
    if (!s.useArea || s.area.isEmpty() || (s.type != Export && s.type != Convert)) return QString();
    return QStringLiteral(", area %1, %2 %3 x %4").arg(s.area.x()).arg(s.area.y()).arg(s.area.width()).arg(s.area.height());
}

QString Step::summary() const {
    if ((type == Export || type == Convert) && useArea && !area.isEmpty()) {
        Step plain = *this;
        plain.useArea = false;
        const QString t = plain.summary();
        return t + QStringLiteral(" (") + areaText(*this).mid(2) + QStringLiteral(")");
    }
    switch (type) {
    case Export: {
        if (!preset.isEmpty()) return QStringLiteral("Export: %1").arg(preset);
        const ExportOptions o = lwd::exportOptionsFromJson(options);
        return QStringLiteral("Export: %1 (custom settings)").arg(ImageIO::formatName(o.format));
    }
    case Scale: {
        const QString rs = resample >= 0 ? QStringLiteral(", ") + pixelscale::resampleNames().value(resample) : QString();
        if (scaleMode == 1) return QStringLiteral("Scale: %1%%2").arg(percent).arg(rs);
        if (scaleMode == 2) return QStringLiteral("Scale: fit within %1 x %2%3%4").arg(width).arg(height).arg(onlyShrink ? QStringLiteral(" (only shrink)") : QString(), rs);
        return QStringLiteral("Scale: %1 x %2%3").arg(width > 0 ? QString::number(width) : QStringLiteral("auto"), height > 0 ? QString::number(height) : QStringLiteral("auto"), rs);
    }
    case ReplaceColor: return QStringLiteral("Replace color %1 -> %2%3").arg(colorName(from), colorName(to), tolerance ? QStringLiteral(" (tolerance %1)").arg(tolerance) : QString());
    case ReplaceText: return QStringLiteral("Replace text \"%1\" -> \"%2\"").arg(find, replace);
    case Rename: {
        QString s = prefix + (baseName.isEmpty() ? QStringLiteral("<name>") : baseName) + suffix;
        if (numbering) s += QStringLiteral("_") + QString::number(start).rightJustified(digits, QLatin1Char('0'));
        return QStringLiteral("Rename: %1").arg(s);
    }
    case Command: {
        const int i = commandKeys().indexOf(command);
        return QStringLiteral("Command: %1").arg(i >= 0 ? commandLabels()[i] : command);
    }
    case Filters: {
        QStringList n;
        for (const LiveEffect &e : effects) n << effectName(e.type);
        return QStringLiteral("Filters: %1").arg(n.isEmpty() ? QStringLiteral("(none)") : n.join(QStringLiteral(", ")));
    }
    case Convert: {
        const bool native = format.compare(QLatin1String("lwd"), Qt::CaseInsensitive) == 0
            || format.compare(QLatin1String("bon"), Qt::CaseInsensitive) == 0
            || format.compare(brand::docExtNoDot(), Qt::CaseInsensitive) == 0;
        return QStringLiteral("Convert to %1").arg(native ? brand::docFilterName()
                                                          : ImageIO::formatName(ImageIO::formatFromKey(format)));
    }
    }
    return QString();
}

QJsonObject Step::toJson() const {
    QJsonObject j;
    j[QStringLiteral("type")] = stepTypeKey(type);
    if (!enabled) j[QStringLiteral("enabled")] = false;
    switch (type) {
    case Export:
        if (!preset.isEmpty()) j[QStringLiteral("preset")] = preset;
        else j[QStringLiteral("options")] = options;
        break;
    case Scale:
        j[QStringLiteral("mode")] = scaleMode == 0 ? QStringLiteral("size") : scaleMode == 1 ? QStringLiteral("percent") : QStringLiteral("fit");
        if (scaleMode == 1) j[QStringLiteral("percent")] = percent;
        else { j[QStringLiteral("width")] = width; j[QStringLiteral("height")] = height; }
        if (scaleMode == 2) j[QStringLiteral("onlyShrink")] = onlyShrink;
        if (resample >= 0) j[QStringLiteral("resample")] = pixelscale::resampleKey(resample);
        break;
    case ReplaceColor:
        j[QStringLiteral("from")] = colorName(from); j[QStringLiteral("to")] = colorName(to);
        j[QStringLiteral("tolerance")] = tolerance;
        break;
    case ReplaceText:
        j[QStringLiteral("find")] = find; j[QStringLiteral("replace")] = replace; j[QStringLiteral("caseSensitive")] = caseSensitive;
        break;
    case Rename:
        j[QStringLiteral("prefix")] = prefix; j[QStringLiteral("suffix")] = suffix;
        if (!baseName.isEmpty()) j[QStringLiteral("name")] = baseName;
        j[QStringLiteral("numbering")] = numbering;
        if (numbering) { j[QStringLiteral("start")] = start; j[QStringLiteral("digits")] = digits; }
        break;
    case Command: j[QStringLiteral("command")] = command; break;
    case Filters: {
        QJsonArray a;
        for (const LiveEffect &e : effects) a.append(lwd::effectToJson(e));
        j[QStringLiteral("effects")] = a;
        break;
    }
    case Convert: j[QStringLiteral("format")] = format; break;
    }
    if ((type == Export || type == Convert) && useArea && !area.isEmpty())
        j[QStringLiteral("area")] = QJsonArray{area.x(), area.y(), area.width(), area.height()};
    return j;
}

static QColor parseColor(const QJsonValue &v, const QColor &def) {
    const QColor c(v.toString());
    return c.isValid() ? c : def;
}

Step Step::fromJson(const QJsonObject &j, bool *ok) {
    Step s;
    s.type = stepTypeFromKey(j[QStringLiteral("type")].toString());
    if (ok) *ok = s.type >= 0;
    if (s.type < 0) { s.type = Export; return s; }
    s.enabled = j[QStringLiteral("enabled")].toBool(true);
    switch (s.type) {
    case Export:
        s.preset = j[QStringLiteral("preset")].toString();
        s.options = j[QStringLiteral("options")].toObject();
        break;
    case Scale: {
        const QString m = j[QStringLiteral("mode")].toString(QStringLiteral("percent"));
        s.scaleMode = m == QLatin1String("size") ? 0 : m == QLatin1String("fit") ? 2 : 1;
        s.percent = j[QStringLiteral("percent")].toDouble(s.percent);
        s.width = qMax(0, j[QStringLiteral("width")].toInt());
        s.height = qMax(0, j[QStringLiteral("height")].toInt());
        s.onlyShrink = j[QStringLiteral("onlyShrink")].toBool();
        s.resample = pixelscale::resampleFromKey(j[QStringLiteral("resample")].toString(), -1);
        break;
    }
    case ReplaceColor:
        s.from = parseColor(j[QStringLiteral("from")], s.from); s.to = parseColor(j[QStringLiteral("to")], s.to);
        s.tolerance = qBound(0, j[QStringLiteral("tolerance")].toInt(), 255);
        break;
    case ReplaceText:
        s.find = j[QStringLiteral("find")].toString(); s.replace = j[QStringLiteral("replace")].toString();
        s.caseSensitive = j[QStringLiteral("caseSensitive")].toBool(true);
        break;
    case Rename:
        s.prefix = j[QStringLiteral("prefix")].toString(); s.suffix = j[QStringLiteral("suffix")].toString();
        s.baseName = j[QStringLiteral("name")].toString();
        s.numbering = j[QStringLiteral("numbering")].toBool();
        s.start = j[QStringLiteral("start")].toInt(1); s.digits = qBound(1, j[QStringLiteral("digits")].toInt(3), 9);
        break;
    case Command: s.command = j[QStringLiteral("command")].toString(); break;
    case Filters:
        for (const QJsonValue &v : j[QStringLiteral("effects")].toArray()) {
            bool eok = false;
            const LiveEffect e = lwd::effectFromJson(v.toObject(), &eok);
            if (eok) s.effects << e;
        }
        break;
    case Convert: s.format = j[QStringLiteral("format")].toString(); break;
    }
    if (s.type == Export || s.type == Convert) {
        const QJsonArray a = j[QStringLiteral("area")].toArray();
        if (a.size() == 4) {
            s.area = QRect(a[0].toInt(), a[1].toInt(), qMax(0, a[2].toInt()), qMax(0, a[3].toInt()));
            s.useArea = !s.area.isEmpty();
        }
    }
    return s;
}

QJsonObject Script::toJson() const {
    QJsonObject j;
    j[QStringLiteral("libreworksBatch")] = 1;
    j[QStringLiteral("input")] = input == Files ? QStringLiteral("files") : input == Folder ? QStringLiteral("folder") : QStringLiteral("open");
    if (!files.isEmpty()) j[QStringLiteral("files")] = QJsonArray::fromStringList(files);
    if (!folder.isEmpty()) j[QStringLiteral("folder")] = folder;
    j[QStringLiteral("recursive")] = recursive;
    j[QStringLiteral("patterns")] = patterns;
    QJsonArray a;
    for (const Step &s : steps) a.append(s.toJson());
    j[QStringLiteral("steps")] = a;
    j[QStringLiteral("output")] = output == ToFolder ? QStringLiteral("folder") : output == Overwrite ? QStringLiteral("overwrite") : QStringLiteral("backup");
    if (!outDir.isEmpty()) j[QStringLiteral("outDir")] = outDir;
    j[QStringLiteral("backupDir")] = backupDir;
    return j;
}

Script Script::fromJson(const QJsonObject &j, QString *err) {
    Script s;
    const QString in = j[QStringLiteral("input")].toString(QStringLiteral("files"));
    s.input = in == QLatin1String("folder") ? Folder : in == QLatin1String("open") ? OpenDocuments : Files;
    for (const QJsonValue &v : j[QStringLiteral("files")].toArray()) s.files << v.toString();
    s.folder = j[QStringLiteral("folder")].toString();
    s.recursive = j[QStringLiteral("recursive")].toBool();
    if (j.contains(QStringLiteral("patterns"))) s.patterns = j[QStringLiteral("patterns")].toString();
    int n = 0;
    for (const QJsonValue &v : j[QStringLiteral("steps")].toArray()) {
        ++n;
        bool ok = false;
        const Step st = Step::fromJson(v.toObject(), &ok);
        if (ok) s.steps << st;
        else if (err) *err += QStringLiteral("Step %1: unknown type \"%2\"\n").arg(n).arg(v.toObject()[QStringLiteral("type")].toString());
    }
    const QString out = j[QStringLiteral("output")].toString(QStringLiteral("folder"));
    s.output = out == QLatin1String("overwrite") ? Overwrite : out == QLatin1String("backup") ? Backup : ToFolder;
    s.outDir = j[QStringLiteral("outDir")].toString();
    if (j.contains(QStringLiteral("backupDir"))) s.backupDir = j[QStringLiteral("backupDir")].toString();
    return s;
}

bool Script::save(const QString &path, QString *err) const {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (err) *err = f.errorString(); return false; }
    f.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    return true;
}

Script Script::load(const QString &path, QString *err, bool *ok) {
    if (ok) *ok = false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return {}; }
    QJsonParseError pe;
    const QJsonDocument jd = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!jd.isObject()) { if (err) *err = QStringLiteral("Not a batch script: %1").arg(pe.errorString()); return {}; }
    QString e;
    Script s = fromJson(jd.object(), &e);
    // relative folders are relative to the script
    const QDir base = QFileInfo(path).absoluteDir();
    if (!s.outDir.isEmpty() && QDir::isRelativePath(s.outDir)) s.outDir = base.absoluteFilePath(s.outDir);
    if (!s.folder.isEmpty() && QDir::isRelativePath(s.folder)) s.folder = base.absoluteFilePath(s.folder);
    for (QString &fi : s.files) if (QDir::isRelativePath(fi)) fi = base.absoluteFilePath(fi);
    if (err) *err = e.trimmed();
    if (ok) *ok = true;
    return s;
}

QStringList inputFiles(const Script &s) {
    if (s.input == Script::Files) return s.files;
    if (s.input != Script::Folder || s.folder.isEmpty()) return {};
    QStringList filters = s.patterns.split(QRegularExpression(QStringLiteral("[\\s;,]+")), Qt::SkipEmptyParts);
    if (filters.isEmpty()) filters << QStringLiteral("*");
    QStringList out;
    QDirIterator it(s.folder, filters, QDir::Files | QDir::Readable, s.recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString p = it.next();
        // never re-process our own backups
        if (!s.backupDir.isEmpty() && QFileInfo(p).absolutePath().endsWith(QLatin1Char('/') + s.backupDir)) continue;
        out << p;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// ---- in-place steps

template <typename F> static void forEachObject(Document &doc, F fn) {
    for (Frame &f : doc.s.frames)
        for (auto &l : f.layers)
            for (VObject &o : l) fn(o);
}

static void canvasTransform(Document &doc, int how) {
    const double W = doc.s.width, H = doc.s.height;
    QTransform t;
    switch (how) {
    case 0: t = QTransform(-1, 0, 0, 1, W, 0); break;            // flip horizontal
    case 1: t = QTransform(1, 0, 0, -1, 0, H); break;            // flip vertical
    case 2: t = QTransform(0, 1, -1, 0, H, 0); break;            // 90 CW: (x,y) -> (H - y, x)
    case 3: t = QTransform(-1, 0, 0, -1, W, H); break;           // 180
    case 4: t = QTransform(0, -1, 1, 0, 0, W); break;            // 90 CCW
    }
    forEachObject(doc, [&](VObject &o) { xf::transformObject(o, t); });
    if (how == 2 || how == 4) std::swap(doc.s.width, doc.s.height);
}

static void mapBitmaps(Document &doc, const std::function<QImage(const QImage &)> &fn) {
    forEachObject(doc, [&](VObject &o) {
        if (o.type != ObjType::Bitmap || o.image.isNull()) return;
        xf::bakeBitmap(o);
        o.image = fn(o.image).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    });
}

bool applyStep(Document &doc, const Step &st, QString *err) {
    auto fail = [err](const QString &m) { if (err) *err = m; return false; };
    switch (st.type) {
    case Scale: {
        const int W = doc.s.width, H = doc.s.height;
        if (W <= 0 || H <= 0) return fail(QStringLiteral("empty canvas"));
        int nw, nh;
        if (st.scaleMode == 1) {
            if (st.percent <= 0) return fail(QStringLiteral("scale percent must be > 0"));
            nw = qMax(1, qRound(W * st.percent / 100.0)); nh = qMax(1, qRound(H * st.percent / 100.0));
        } else if (st.scaleMode == 0) {
            if (st.width <= 0 && st.height <= 0) return fail(QStringLiteral("scale needs a width or a height"));
            nw = st.width > 0 ? st.width : qMax(1, qRound(double(W) * st.height / H));
            nh = st.height > 0 ? st.height : qMax(1, qRound(double(H) * st.width / W));
        } else {
            if (st.width <= 0 || st.height <= 0) return fail(QStringLiteral("fit needs a width and a height"));
            double k = qMin(double(st.width) / W, double(st.height) / H);
            if (st.onlyShrink) k = qMin(k, 1.0);
            nw = qMax(1, qRound(W * k)); nh = qMax(1, qRound(H * k));
        }
        if (nw != W || nh != H) doc.scaleImage(nw, nh, st.resample);
        return true;
    }
    case ReplaceColor: {   // the Find & Replace engine: fills, strokes, gradient stops, bitmap pixels, canvas
        fr::Query q;
        q.what = fr::What::Color;
        q.from = st.from; q.to = st.to; q.tolerance = st.tolerance;
        q.effects = false; q.pixels = true; q.canvas = true;
        fr::replaceAllInState(doc.s, q);
        return true;
    }
    case ReplaceText: {
        if (st.find.isEmpty()) return fail(QStringLiteral("nothing to find"));
        fr::Query q;
        q.what = fr::What::Text;
        q.find = st.find; q.replace = st.replace; q.caseSensitive = st.caseSensitive;
        fr::replaceAllInState(doc.s, q);
        return true;
    }
    case Command: {
        const int c = commandKeys().indexOf(st.command);
        switch (c) {
        case 0: case 1: case 2: case 3: case 4: canvasTransform(doc, c); return true;
        case 5: {   // trim: the opaque pixels of every frame
            QRect r;
            for (int i = 0; i < doc.s.frames.size(); ++i) r = r.united(filters::opaqueBounds(doc.render(i, false)));
            if (!r.isEmpty() && r != QRect(0, 0, doc.s.width, doc.s.height)) doc.resizeCanvas(r.width(), r.height(), -r.topLeft());
            return true;
        }
        case 6: {
            const QRect r = doc.contentPixelBounds();
            if (!r.isEmpty() && r != QRect(0, 0, doc.s.width, doc.s.height)) doc.resizeCanvas(r.width(), r.height(), -r.topLeft());
            return true;
        }
        case 7:
            if (doc.s.layers.size() < 2) return true;
            for (auto &f : doc.s.frames) {
                QVector<VObject> all;
                for (int li = 0; li < f.layers.size(); ++li) if (doc.s.layers[li].visible) all += f.layers[li];
                f.layers = {all};
            }
            doc.s.layers = {doc.s.layers.first()};
            doc.s.layers[0].visible = true;
            doc.s.layers[0].locked = false;
            doc.curLayer = 0;
            return true;
        case 8:
            mapBitmaps(doc, [](const QImage &in) {
                QImage im = in.convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < im.height(); ++y) {
                    QRgb *p = reinterpret_cast<QRgb *>(im.scanLine(y));
                    for (int x = 0; x < im.width(); ++x) { const int g = qGray(p[x]); p[x] = qRgba(g, g, g, qAlpha(p[x])); }
                }
                return im;
            });
            forEachObject(doc, [](VObject &o) {
                auto grey = [](QColor &col) { if (col.isValid()) { const int g = qGray(col.rgb()); col = QColor(g, g, g, col.alpha()); } };
                grey(o.fill); grey(o.stroke);
                for (GradStop &s : o.gradient) grey(s.color);
            });
            return true;
        case 9: {
            LiveEffect e = LiveEffect::make(EffectType::Invert);
            mapBitmaps(doc, [&e](const QImage &in) { QImage x = in; fx::adjust(x, e); return x; });
            forEachObject(doc, [](VObject &o) {
                auto inv = [](QColor &col) { if (col.isValid()) col = QColor(255 - col.red(), 255 - col.green(), 255 - col.blue(), col.alpha()); };
                inv(o.fill); inv(o.stroke);
                for (GradStop &s : o.gradient) inv(s.color);
            });
            return true;
        }
        case 10: mapBitmaps(doc, [](const QImage &in) { return fx2::autoContrast(in); }); return true;
        }
        return fail(QStringLiteral("unknown command \"%1\"").arg(st.command));
    }
    case Filters:
        // adjustments are baked into bitmaps in stack order; other effects (and everything on vector
        // objects) are added as live effects so vectors stay editable
        forEachObject(doc, [&](VObject &o) {
            if (o.type == ObjType::Slice || o.type == ObjType::Hotspot) return;
            if (o.type == ObjType::Bitmap && !o.image.isNull()) {
                xf::bakeBitmap(o);
                for (const LiveEffect &e : st.effects) {
                    if (!e.enabled) continue;
                    if (effectIsAdjustment(e.type)) {
                        QImage x = o.image;
                        fx::adjust(x, e);
                        o.image = x.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    } else {
                        o.effects << e;
                    }
                }
            } else {
                for (const LiveEffect &e : st.effects) if (e.enabled) o.effects << e;
            }
        });
        return true;
    default:
        return true;
    }
}

// ---- running

QString outputPath(const Script &s, const QString &sourcePath, const QString &base, const QString &ext) {
    const QFileInfo fi(sourcePath);
    QString dir = s.output == Script::ToFolder ? s.outDir : fi.absolutePath();
    if (dir.isEmpty()) dir = fi.absolutePath();
    return QDir(dir).filePath(base + QLatin1Char('.') + ext);
}

static bool prepareTarget(const Script &s, const QString &path, Result &res, QString *err) {
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) { *err = QStringLiteral("cannot create folder %1").arg(fi.absolutePath()); return false; }
    if (s.output == Script::Backup && fi.exists()) {
        const QDir bdir(QDir(fi.absolutePath()).filePath(s.backupDir.isEmpty() ? QStringLiteral("Original Files") : s.backupDir));
        if (!QDir().mkpath(bdir.absolutePath())) { *err = QStringLiteral("cannot create backup folder"); return false; }
        QString b = bdir.filePath(fi.fileName());
        for (int n = 2; QFileInfo::exists(b); ++n) b = bdir.filePath(QStringLiteral("%1 (%2).%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix()));
        if (!QFile::copy(path, b)) { *err = QStringLiteral("cannot back up %1").arg(path); return false; }
        res.log << QStringLiteral("backup: %1").arg(b);
    }
    return true;
}

static bool writeDoc(const Script &s, Document &doc, const QString &src, const QString &base, const ExportOptions &o, bool lwdFile,
                     Result &res, QString *err) {
    const QString ext = lwdFile ? brand::docExtNoDot() : ImageIO::extension(o.format);
    const QString path = outputPath(s, src, base, ext);
    if (!prepareTarget(s, path, res, err)) return false;
    bool ok;
    if (lwdFile) ok = lwd::save(doc, path, err);
    else ok = docio::exportDocumentFile(doc, doc.curFrame, o, path, err);
    if (ok) { res.written << path; res.log << QStringLiteral("wrote: %1").arg(path); }
    return ok;
}

bool runOnDocument(const Script &s, Document &doc, const QString &src, int index, Result &res) {
    QString base = QFileInfo(src).completeBaseName();
    if (base.isEmpty()) base = QStringLiteral("untitled");
    bool wrote = false;
    for (int i = 0; i < s.steps.size(); ++i) {
        const Step &st = s.steps[i];
        if (!st.enabled) continue;
        QString err;
        bool ok = true;
        switch (st.type) {
        case Export: {
            ExportOptions o = doc.exportOptions;
            if (!st.preset.isEmpty()) {
                const int p = exportpresets::indexOf(st.preset);
                if (p < 0) { err = QStringLiteral("unknown export preset \"%1\"").arg(st.preset); ok = false; break; }
                o = exportpresets::apply(p, o);
            } else if (!st.options.isEmpty()) {
                o = lwd::exportOptionsFromJson(st.options);
            }
            if (st.useArea && !st.area.isEmpty()) o.area = st.area;
            ok = writeDoc(s, doc, src, base, o, false, res, &err);
            wrote = true;
            break;
        }
        case Convert: {
            const bool lwdFile = st.format.compare(QLatin1String("lwd"), Qt::CaseInsensitive) == 0
                || st.format.compare(brand::docExtNoDot(), Qt::CaseInsensitive) == 0
                || st.format.compare(QLatin1String("bon"), Qt::CaseInsensitive) == 0;
            ExportOptions o = doc.exportOptions;
            if (!lwdFile) {
                Fmt f = ImageIO::formatFromKey(st.format);
                if (f == Fmt::Unknown) f = ImageIO::fromExtension(st.format);
                if (f == Fmt::Unknown || !ImageIO::canWrite(f)) { err = QStringLiteral("cannot write format \"%1\"").arg(st.format); ok = false; break; }
                o.format = f;
            }
            if (st.useArea && !st.area.isEmpty() && !lwdFile) o.area = st.area;
            ok = writeDoc(s, doc, src, base, o, lwdFile, res, &err);
            wrote = true;
            break;
        }
        case Rename: {
            QString b = st.baseName.isEmpty() ? base : st.baseName;
            b = st.prefix + b + st.suffix;
            if (st.numbering) b += QStringLiteral("_") + QString::number(st.start + index).rightJustified(st.digits, QLatin1Char('0'));
            b.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
            base = b;
            break;
        }
        default:
            ok = applyStep(doc, st, &err);
        }
        if (!ok) {
            res.errors << QStringLiteral("%1: step %2 (%3): %4").arg(QFileInfo(src).fileName()).arg(i + 1).arg(st.summary(), err);
            return false;
        }
    }
    if (!wrote) {
        // no export step: save in the source's own format
        QString err;
        const Fmt f = ImageIO::fromExtension(src);
        const bool lwdFile = brand::isNativeDocumentPath(src) || f == Fmt::Unknown || !ImageIO::canWrite(f);
        ExportOptions o = doc.exportOptions;
        if (!lwdFile) o.format = f;
        if (!writeDoc(s, doc, src, base, o, lwdFile, res, &err)) {
            res.errors << QStringLiteral("%1: save: %2").arg(QFileInfo(src).fileName(), err);
            return false;
        }
    }
    return true;
}

Result run(const Script &s, const QStringList &filesIn, const Progress &progress, const std::function<Document *(const QString &)> &openDoc) {
    Result res;
    const QStringList files = filesIn.isEmpty() ? inputFiles(s) : filesIn;
    if (s.output == Script::ToFolder && s.outDir.isEmpty()) {
        res.errors << QStringLiteral("No output folder set.");
        res.failed = files.size();
        return res;
    }
    for (int i = 0; i < files.size(); ++i) {
        const QString &f = files[i];
        if (progress && !progress(i, files.size(), f)) { res.cancelled = true; break; }
        QString err;
        Document *doc = nullptr;
        if (openDoc) doc = openDoc(f);
        if (!doc) {
            if (brand::isNativeDocumentPath(f)) doc = lwd::load(f, &err);
            else doc = docio::open(f, &err);
        }
        if (!doc) { res.errors << QStringLiteral("%1: %2").arg(QFileInfo(f).fileName(), err.isEmpty() ? QStringLiteral("cannot open") : err); ++res.failed; continue; }
        if (runOnDocument(s, *doc, f, i, res)) ++res.processed; else ++res.failed;
        delete doc;
    }
    if (progress && !res.cancelled) progress(files.size(), files.size(), QString());
    return res;
}
}
