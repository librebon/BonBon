#include "core/FindReplace.h"
#include "core/Document.h"
#include <QRegularExpression>
#include <functional>

namespace fr {

QStringList whatNames() {
    return {QStringLiteral("Find Text"), QStringLiteral("Find Font"), QStringLiteral("Find Color"), QStringLiteral("Find URL"),
            QStringLiteral("Find Non-Web216")};
}
QStringList scopeNames() {
    return {QStringLiteral("Search Document"), QStringLiteral("Search Selection"), QStringLiteral("Search Frames"),
            QStringLiteral("Search All Open Documents")};
}

bool isWebSafe(QRgb c) { return qRed(c) % 0x33 == 0 && qGreen(c) % 0x33 == 0 && qBlue(c) % 0x33 == 0; }
QRgb nearestWebSafe(QRgb c) {
    auto q = [](int v) { return ((v + 0x19) / 0x33) * 0x33; };
    return qRgba(q(qRed(c)), q(qGreen(c)), q(qBlue(c)), qAlpha(c));
}

namespace {
bool near(QRgb a, QRgb b, int tol) {
    return qAbs(qRed(a) - qRed(b)) <= tol && qAbs(qGreen(a) - qGreen(b)) <= tol && qAbs(qBlue(a) - qBlue(b)) <= tol;
}
bool colorQuery(const Query &q) { return q.what == What::Color || q.what == What::NonWebColor; }

bool fieldMatches(const QColor &c, const Query &q) {
    if (!c.isValid() || c.alpha() == 0) return false;
    if (q.what == What::NonWebColor) return !isWebSafe(c.rgb());
    return near(c.rgb(), q.from.rgb(), q.tolerance);
}
// Batch semantics: To's colour, the alpha scaled by To's alpha
bool replaceField(QColor &c, const Query &q) {
    if (!fieldMatches(c, q)) return false;
    if (q.what == What::NonWebColor) {
        const QRgb n = nearestWebSafe(c.rgba());
        if (n == c.rgba()) return false;
        c = QColor::fromRgba(n);
        return true;
    }
    const int a = c.alpha();
    const QColor before = c;
    c = q.to;
    c.setAlpha(a * q.to.alpha() / 255);
    return c != before;
}

bool effectUsesColor(EffectType t) {
    switch (t) {
    case EffectType::DropShadow: case EffectType::InnerShadow: case EffectType::OuterGlow: case EffectType::InnerGlow:
    case EffectType::InnerBevel: case EffectType::OuterBevel: case EffectType::ColorFill: case EffectType::PhotoFilter:
    case EffectType::Outline: case EffectType::ReplaceColor:
        return true;
    default: return false;
    }
}

// Every colour field of an object the query covers. `visibleOnly` skips fields that do not draw.
template <typename O, typename F> void eachColor(O &o, const Query &q, bool visibleOnly, F fn) {
    if (visibleOnly && (o.type == ObjType::Slice || o.type == ObjType::Hotspot)) return;   // web objects: not artwork
    if (o.type != ObjType::Bitmap) {
        if (q.fills && (!visibleOnly || o.fillOn)) {
            if (!visibleOnly || o.fillKind == FillKind::Solid || o.gradient.isEmpty()) fn(o.fill, QStringLiteral("Fill"));
        }
        if (q.strokes && (!visibleOnly || o.strokeOn)) fn(o.stroke, QStringLiteral("Stroke"));
        if (q.gradients && (!visibleOnly || (o.fillOn && o.fillKind != FillKind::Solid && o.fillKind != FillKind::WebDither && o.fillKind != FillKind::Pattern)))
            for (auto &g : o.gradient) fn(g.color, QStringLiteral("Gradient stop"));
    }
    if (q.effects) {
        for (auto &e : o.effects) {
            if (visibleOnly && (!e.enabled || !(effectUsesColor(e.type) || e.type == EffectType::GradientMap))) continue;
            const QString n = effectName(e.type);
            if (!visibleOnly || effectUsesColor(e.type)) {
                fn(e.color, n);
                if (!visibleOnly || e.type == EffectType::ReplaceColor) fn(e.color2, n);
            }
            if (!visibleOnly || e.type == EffectType::GradientMap)
                for (auto &g : e.stops) fn(g.color, n);
        }
    }
}

bool pixelsMatch(const QImage &img, const Query &q) {
    const QImage im = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < im.height(); ++y) {
        const QRgb *p = reinterpret_cast<const QRgb *>(im.constScanLine(y));
        for (int x = 0; x < im.width(); ++x) {
            if (qAlpha(p[x]) == 0) continue;
            if (q.what == What::NonWebColor ? !isWebSafe(p[x]) : near(p[x], q.from.rgb(), q.tolerance)) return true;
        }
    }
    return false;
}
int replacePixels(QImage &img, const Query &q) {
    QImage im = img.convertToFormat(QImage::Format_ARGB32);
    const QRgb to = q.to.rgb();
    int n = 0;
    for (int y = 0; y < im.height(); ++y) {
        QRgb *p = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < im.width(); ++x) {
            if (qAlpha(p[x]) == 0) continue;
            if (q.what == What::NonWebColor) {
                if (isWebSafe(p[x])) continue;
                p[x] = nearestWebSafe(p[x]);
                ++n;
            } else if (near(p[x], q.from.rgb(), q.tolerance)) {
                const QRgb v = qRgba(qRed(to), qGreen(to), qBlue(to), qAlpha(p[x]) * q.to.alpha() / 255);
                if (v != p[x]) { p[x] = v; ++n; }
            }
        }
    }
    if (n) img = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return n;
}

QRegularExpression textRx(const Query &q) {
    QString pat = q.regex ? q.find : QRegularExpression::escape(q.find);
    if (q.wholeWord) pat = QStringLiteral("\\b(?:%1)\\b").arg(pat);
    return QRegularExpression(pat, q.caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
}
bool simpleText(const Query &q) { return !q.regex && !q.wholeWord; }

int countIn(const QString &s, const Query &q) {
    if (q.find.isEmpty() || s.isEmpty()) return 0;
    if (simpleText(q)) return s.count(q.find, q.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    const QRegularExpression rx = textRx(q);
    if (!rx.isValid()) return 0;
    int n = 0;
    for (auto it = rx.globalMatch(s); it.hasNext();) { const auto m = it.next(); if (m.capturedLength() > 0) ++n; }
    return n;
}
int replaceString(QString &s, const Query &q) {
    const int n = countIn(s, q);
    if (!n) return 0;
    const QString before = s;
    if (simpleText(q)) s.replace(q.find, q.replace, q.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    else s.replace(textRx(q), q.replace);
    return s == before ? 0 : n;
}
QString shortText(const QString &t) {
    QString s = t.simplified();
    if (s.size() > 40) s = s.left(39) + QChar(0x2026);
    return s;
}
bool fontMatches(const VObject &o, const Query &q) {
    if (o.type != ObjType::Text) return false;
    if (!q.fontFrom.isEmpty() && o.fontFamily.compare(q.fontFrom, Qt::CaseInsensitive) != 0) return false;
    if (q.sizeFrom > 0 && std::abs(o.fontSize - q.sizeFrom) > 0.01) return false;
    return true;
}
}

QString matchIn(const VObject &o, const Query &q) {
    switch (q.what) {
    case What::Text: {
        if (o.type != ObjType::Text) return {};
        const int n = countIn(o.text, q);
        return n ? QStringLiteral("Text \u201c%1\u201d (%2)").arg(shortText(o.text)).arg(n) : QString();
    }
    case What::Font:
        return fontMatches(o, q) ? QStringLiteral("Font %1 %2 pt").arg(o.fontFamily).arg(o.fontSize) : QString();
    case What::Url: {
        if (o.link.isEmpty()) return {};
        if (q.find.isEmpty()) return QStringLiteral("URL %1").arg(o.link);   // empty = any link
        return countIn(o.link, q) ? QStringLiteral("URL %1").arg(o.link) : QString();
    }
    case What::Color: case What::NonWebColor: {
        QStringList where;
        QColor dummyFound;
        eachColor(o, q, true, [&](const QColor &c, const QString &n) {
            if (fieldMatches(c, q) && !where.contains(n)) { where << n; if (!dummyFound.isValid()) dummyFound = c; }
        });
        if (q.pixels && o.type == ObjType::Bitmap && !o.image.isNull() && pixelsMatch(o.image, q)) where << QStringLiteral("Pixels");
        if (where.isEmpty()) return {};
        return where.join(QStringLiteral(", ")) + (dummyFound.isValid() ? QStringLiteral(" ") + dummyFound.name() : QString());
    }
    }
    return {};
}

static int replaceInImpl(VObject &o, const Query &q, bool visibleOnly) {
    switch (q.what) {
    case What::Text: return o.type == ObjType::Text ? replaceString(o.text, q) : 0;
    case What::Font: {
        if (!fontMatches(o, q)) return 0;
        bool ch = false;
        if (!q.fontTo.isEmpty() && o.fontFamily != q.fontTo) { o.fontFamily = q.fontTo; ch = true; }
        if (q.sizeTo > 0 && std::abs(o.fontSize - q.sizeTo) > 1e-9) { o.fontSize = q.sizeTo; ch = true; }
        return ch ? 1 : 0;
    }
    case What::Url:
        if (o.link.isEmpty()) return 0;
        if (q.find.isEmpty()) { if (o.link == q.replace) return 0; o.link = q.replace; return 1; }
        return replaceString(o.link, q);
    case What::Color: case What::NonWebColor: {
        int n = 0;
        eachColor(o, q, visibleOnly, [&](QColor &c, const QString &) { n += replaceField(c, q) ? 1 : 0; });
        if (q.pixels && o.type == ObjType::Bitmap && !o.image.isNull()) n += replacePixels(o.image, q) > 0 ? 1 : 0;
        return n;
    }
    }
    return 0;
}

int replaceIn(VObject &o, const Query &q) { return replaceInImpl(o, q, true); }

QVector<Hit> findAll(const Document &doc, const Query &q, Scope scope, const QSet<quint64> *selection) {
    QVector<Hit> hits;
    const bool all = scope == Scope::AllFrames || scope == Scope::AllDocuments;
    for (int f = 0; f < doc.s.frames.size(); ++f) {
        if (!all && f != doc.curFrame) continue;
        const Frame &fr = doc.s.frames[f];
        for (int l = 0; l < fr.layers.size(); ++l) {
            if (l < doc.s.layers.size() && doc.s.layers[l].locked) continue;
            if (f != doc.curFrame && l < doc.s.layers.size() && doc.s.layers[l].shared) continue;   // a shared layer once
            const QSet<quint64> &sel = selection ? *selection : doc.selection;
            // v0.3.2: groups are searched into (members and masks); a member is in the Selection scope when
            // it or a group holding it is selected; a locked group locks its contents
            std::function<void(const VObject &, int, bool)> visit = [&](const VObject &o, int i, bool inSel) {
                if (o.locked) return;
                inSel = inSel || sel.contains(o.id);
                if (scope != Scope::Selection || inSel) {
                    const QString d = matchIn(o, q);
                    if (!d.isEmpty()) hits << Hit{f, l, i, o.id, d};
                }
                for (const VObject &c : o.children) visit(c, i, inSel);
                for (const VObject &m : o.maskObj) visit(m, i, inSel);
            };
            for (int i = 0; i < fr.layers[l].size(); ++i) visit(fr.layers[l][i], i, false);
        }
    }
    return hits;
}

int replaceHit(Document &doc, const Query &q, const Hit &h) {
    if (h.frame < 0 || h.frame >= doc.s.frames.size()) return 0;
    Frame &fr = doc.s.frames[h.frame];
    std::function<VObject *(VObject &)> findIn = [&](VObject &o) -> VObject * {
        if (o.id == h.objectId) return &o;
        for (VObject &c : o.children) if (VObject *r = findIn(c)) return r;
        for (VObject &m : o.maskObj) if (VObject *r = findIn(m)) return r;
        return nullptr;
    };
    for (auto &layer : fr.layers)
        for (VObject &o : layer)
            if (VObject *t = findIn(o)) return replaceIn(*t, q);
    return 0;
}

int replaceAll(Document &doc, const Query &q, Scope scope, const QSet<quint64> *selection) {
    int n = 0;
    for (const Hit &h : findAll(doc, q, scope, selection)) n += replaceHit(doc, q, h);
    if (n) doc.syncShared();
    return n;
}

int replaceAllInState(DocState &s, const Query &q) {
    int n = 0;
    for (Frame &f : s.frames)
        for (auto &l : f.layers)
            for (VObject &o : l) o.forEachNested([&](VObject &x) { n += replaceInImpl(x, q, false); });
    if (q.canvas && colorQuery(q) && replaceField(s.canvasColor, q)) ++n;
    return n;
}
}
