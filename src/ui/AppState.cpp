#include "io/ImageIO.h"
#include <QPainter>
#include "AppState.h"
#include "core/Tablet.h"
#include "core/AutoShape.h"
#include "core/AutoShape.h"
#include "Theme.h"
#include "PopupSlider.h"
#include "core/CorePrefs.h"
#include <QList>
#include <QSettings>
#include <QFontDatabase>
#include <QApplication>
#include <QFontInfo>
#include <QFont>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <functional>

AppState &AppState::get() {
    static AppState *s = new AppState;
    return *s;
}

void AppState::setPrefs(const Prefs &p) {
    m_lastPrefsChangedFontSources = p.useSystemFonts != prefs.useSystemFonts
        || p.useCustomFonts != prefs.useCustomFonts
        || p.customFontDir != prefs.customFontDir;
    m_lastPrefsChangedWebFeatures = p.enableWebFeatures != prefs.enableWebFeatures;
    prefs = p;
    prefs.undoSteps = qBound(1, prefs.undoSteps, 999);
    prefs.undoMemoryMB = qBound(64, prefs.undoMemoryMB, 65536);
    prefs.pickDistance = qBound(1, prefs.pickDistance, 20);
    prefs.interpolation = qBound(0, prefs.interpolation, int(coreprefs::LastResample));
    prefs.gridSize = qBound(1, prefs.gridSize, 500);
    prefs.sliderOrientation = qBound(0, prefs.sliderOrientation, 1);
    PopupSliderHub::setOrientation(prefs.sliderOrientation == 1 ? Qt::Vertical : Qt::Horizontal);
    prefs.shiftFineSpeed = qBound(5, prefs.shiftFineSpeed, 100);
    PopupSliderHub::setFineFactor(prefs.shiftFineSpeed / 100.0);
    prefs.ctrlSnapIncrement = qBound(1, prefs.ctrlSnapIncrement, 100);
    PopupSliderHub::setSnapIncrement(prefs.ctrlSnapIncrement);
    coreprefs::gridSize = prefs.gridSize;
    coreprefs::gridColor = prefs.gridColor.rgba();
    prefs.recentFiles = qBound(0, prefs.recentFiles, 30);
    coreprefs::undoSteps = prefs.undoSteps;
    coreprefs::undoMemoryMB = prefs.undoMemoryMB;
    coreprefs::interpolation = prefs.interpolation;
    prefs.customMode = qBound(0, prefs.customMode, 2);
    prefs.customVibrancy = qBound(0, prefs.customVibrancy, 100);
    if (!prefs.customSeed.isValid()) prefs.customSeed = Prefs().customSeed;
    theme::syncGenerated(prefs);   // so "custom" / "user:<name>" ids resolve below
    if (!theme::exists(prefs.theme)) prefs.theme = theme::defaultId();
    if (!prefs.pasteboardOverride) prefs.pasteboardColor = theme::pasteboard(theme::resolve(prefs.theme, prefs.themeFollowSystem));
    theme::applyPrefs(prefs);
    {
        ToneMap tm;
        tm.op = prefs.hdrToneMap;
        tm.exposure = prefs.hdrExposure;
        ImageIO::setToneMap(tm);
    }
    // Reloading application fonts and walking QFontDatabase on every theme
    // switch made setPrefs several times slower than 0.3.27.
    if (m_lastPrefsChangedFontSources)
        applyFontSources();
    if (!prefs.enableWebFeatures && (toolIsHotspot(m_tool) || toolIsSlice(m_tool))) {
        m_tool = Tool::Pointer;
        emit toolChanged(m_tool);
    }
    // Refresh font lists first so a live text object's family is kept, then
    // (if needed) retarget the application font to a fallback face.
    emit prefsChanged();
    if (m_lastPrefsChangedFontSources)
        applyUiFontForSources();
}

void AppState::loadPrefs() {
    QSettings st;
    // v0.2.7: the remembered "strip spaces/tabs/newlines from arrays" choice (C header export), on by default
    cheader_setStripDefault(st.value(QStringLiteral("export/chStrip"), true).toBool());
    st.beginGroup(QStringLiteral("prefs"));
    Prefs p;
    auto col = [&](const char *k, const QColor &def) {
        QColor c(st.value(QLatin1String(k), def.name(QColor::HexArgb)).toString());
        return c.isValid() ? c : def;
    };
    p.undoSteps = st.value(QStringLiteral("undoSteps"), p.undoSteps).toInt();
    p.undoMemoryMB = st.value(QStringLiteral("undoMemoryMB"), p.undoMemoryMB).toInt();
    p.defaultStroke = col("defaultStroke", p.defaultStroke);
    p.defaultFill = col("defaultFill", p.defaultFill);
    p.mouseHighlight = st.value(QStringLiteral("mouseHighlight"), p.mouseHighlight).toBool();
    p.highlightColor = col("highlightColor", p.highlightColor);
    p.selectionColor = col("selectionColor", p.selectionColor);
    {
        const QColor def = p.pasteboardColor;
        p.pasteboardColor = col("pasteboardColor", def);
        // settings from before the Themes tab: a custom pasteboard colour counts as an override
        p.pasteboardOverride = st.value(QStringLiteral("pasteboardOverride"), p.pasteboardColor != def).toBool();
    }
    p.theme = st.value(QStringLiteral("theme"), p.theme).toString();
    p.themeFollowSystem = st.value(QStringLiteral("themeFollowSystem"), p.themeFollowSystem).toBool();
    p.accentOverride = st.value(QStringLiteral("accentOverride"), p.accentOverride).toBool();
    p.accentColor = col("accentColor", p.accentColor);
    p.customSeed = col("customSeed", p.customSeed);
    p.customMode = st.value(QStringLiteral("customMode"), p.customMode).toInt();
    p.customVibrancy = st.value(QStringLiteral("customVibrancy"), p.customVibrancy).toInt();
    for (const QString &js : st.value(QStringLiteral("userThemes")).toStringList()) {
        const QJsonObject o = QJsonDocument::fromJson(js.toUtf8()).object();
        theme::Spec sp;
        sp.name = o.value(QStringLiteral("name")).toString();
        sp.seed = QColor::fromString(o.value(QStringLiteral("seed")).toString());
        sp.mode = qBound(0, o.value(QStringLiteral("mode")).toInt(), 2);
        sp.vibrancy = qBound(0, o.value(QStringLiteral("vibrancy")).toInt(40), 100);
        if (!sp.name.isEmpty() && sp.seed.isValid()) p.userThemes.append(sp);
    }
    p.pickDistance = st.value(QStringLiteral("pickDistance"), p.pickDistance).toInt();
    p.brushSizeCursors = st.value(QStringLiteral("brushSizeCursors"), p.brushSizeCursors).toBool();
    p.preciseCursors = st.value(QStringLiteral("preciseCursors"), p.preciseCursors).toBool();
    p.flyoutHoldMs = qBound(50, st.value(QStringLiteral("flyoutHoldMs"), p.flyoutHoldMs).toInt(), 2000);
    // Vertical by default (0.3.2); an explicit Horizontal choice is kept
    p.sliderOrientation = st.value(QStringLiteral("popupSliderOrientation")).toString() == QLatin1String("horizontal") ? 0 : 1;
    p.shiftFineSpeed = qBound(5, st.value(QStringLiteral("shiftFineSpeedPercent"), p.shiftFineSpeed).toInt(), 100);
    p.ctrlSnapIncrement = qBound(1, st.value(QStringLiteral("ctrlSnapIncrement"), p.ctrlSnapIncrement).toInt(), 100);
    p.strokePreviewBg = qBound(0, st.value(QStringLiteral("strokePreviewBg"), p.strokePreviewBg).toInt(), 2);
    p.interpolation = st.value(QStringLiteral("interpolation"), p.interpolation).toInt();
    p.gridSize = st.value(QStringLiteral("gridSize"), p.gridSize).toInt();
    p.gridColor = col("gridColor", p.gridColor);
    p.checkerboard = st.value(QStringLiteral("checkerboard"), p.checkerboard).toBool();
    p.canvasBorder = st.value(QStringLiteral("canvasBorder"), p.canvasBorder).toBool();
    p.fitMargin = qBound(0, st.value(QStringLiteral("fitMargin"), p.fitMargin).toInt(), 200);
    p.useSystemFonts = st.value(QStringLiteral("useSystemFonts"), p.useSystemFonts).toBool();
    p.useCustomFonts = st.value(QStringLiteral("useCustomFonts"), p.useCustomFonts).toBool();
    p.customFontDir = st.value(QStringLiteral("customFontDir"), p.customFontDir).toString();
    p.carryToolSettings = st.value(QStringLiteral("carryToolSettings"), p.carryToolSettings).toBool();
    p.pasteAtMouse = st.value(QStringLiteral("pasteAtMouse"), p.pasteAtMouse).toBool();
    p.useSystemIcons = st.value(QStringLiteral("useSystemIcons"), p.useSystemIcons).toBool();
    p.useSystemFileDialogs = st.value(QStringLiteral("useSystemFileDialogs"), p.useSystemFileDialogs).toBool();
    // 0.3.22: ignore a leftover "usePolygonSlices" key in an old config.
    p.enableWebFeatures = st.value(QStringLiteral("enableWebFeatures"), p.enableWebFeatures).toBool();
    p.canvasShadow = st.value(QStringLiteral("canvasShadow"), p.canvasShadow).toBool();
    p.snapToCanvas = st.value(QStringLiteral("snapToCanvas"), p.snapToCanvas).toBool();
    p.snapToObjects = st.value(QStringLiteral("snapToObjects"), p.snapToObjects).toBool();
    p.recentFiles = st.value(QStringLiteral("recentFiles"), p.recentFiles).toInt();
    p.hdrToneMap = qBound(0, st.value(QStringLiteral("hdrToneMap"), p.hdrToneMap).toInt(), 3);
    p.hdrExposure = qBound(-10.0, st.value(QStringLiteral("hdrExposure"), p.hdrExposure).toDouble(), 10.0);
    p.tabletCurve = tablet::decodeCurve(st.value(QStringLiteral("tabletCurve"), tablet::encodeCurve(p.tabletCurve)).toString());
    p.tabletMinSize = qBound(0, st.value(QStringLiteral("tabletMinSize"), p.tabletMinSize).toInt(), 100);
    p.tabletMinOpacity = qBound(0, st.value(QStringLiteral("tabletMinOpacity"), p.tabletMinOpacity).toInt(), 100);
    p.tabletEraser = st.value(QStringLiteral("tabletEraser"), p.tabletEraser).toBool();
    st.endGroup();
    setPrefs(p);
}

void AppState::savePrefs() const {
    QSettings st;
    st.beginGroup(QStringLiteral("prefs"));
    const Prefs &p = prefs;
    st.setValue(QStringLiteral("undoSteps"), p.undoSteps);
    st.setValue(QStringLiteral("undoMemoryMB"), p.undoMemoryMB);
    st.setValue(QStringLiteral("defaultStroke"), p.defaultStroke.name(QColor::HexArgb));
    st.setValue(QStringLiteral("defaultFill"), p.defaultFill.name(QColor::HexArgb));
    st.setValue(QStringLiteral("mouseHighlight"), p.mouseHighlight);
    st.setValue(QStringLiteral("highlightColor"), p.highlightColor.name(QColor::HexArgb));
    st.setValue(QStringLiteral("selectionColor"), p.selectionColor.name(QColor::HexArgb));
    st.setValue(QStringLiteral("pasteboardColor"), p.pasteboardColor.name(QColor::HexArgb));
    st.setValue(QStringLiteral("pasteboardOverride"), p.pasteboardOverride);
    st.setValue(QStringLiteral("theme"), p.theme);
    st.setValue(QStringLiteral("themeFollowSystem"), p.themeFollowSystem);
    st.setValue(QStringLiteral("accentOverride"), p.accentOverride);
    st.setValue(QStringLiteral("accentColor"), p.accentColor.name(QColor::HexArgb));
    st.setValue(QStringLiteral("customSeed"), p.customSeed.name(QColor::HexArgb));
    st.setValue(QStringLiteral("customMode"), p.customMode);
    st.setValue(QStringLiteral("customVibrancy"), p.customVibrancy);
    {
        QStringList ut;
        for (const theme::Spec &sp : p.userThemes) {
            QJsonObject o;
            o.insert(QStringLiteral("name"), sp.name);
            o.insert(QStringLiteral("seed"), sp.seed.name());
            o.insert(QStringLiteral("mode"), sp.mode);
            o.insert(QStringLiteral("vibrancy"), sp.vibrancy);
            ut << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
        }
        st.setValue(QStringLiteral("userThemes"), ut);
    }
    st.setValue(QStringLiteral("pickDistance"), p.pickDistance);
    st.setValue(QStringLiteral("brushSizeCursors"), p.brushSizeCursors);
    st.setValue(QStringLiteral("preciseCursors"), p.preciseCursors);
    st.setValue(QStringLiteral("flyoutHoldMs"), p.flyoutHoldMs);
    st.setValue(QStringLiteral("popupSliderOrientation"), p.sliderOrientation == 1 ? QStringLiteral("vertical") : QStringLiteral("horizontal"));
    st.setValue(QStringLiteral("shiftFineSpeedPercent"), p.shiftFineSpeed);
    st.setValue(QStringLiteral("ctrlSnapIncrement"), p.ctrlSnapIncrement);
    st.setValue(QStringLiteral("strokePreviewBg"), p.strokePreviewBg);
    st.setValue(QStringLiteral("interpolation"), p.interpolation);
    st.setValue(QStringLiteral("gridSize"), p.gridSize);
    st.setValue(QStringLiteral("gridColor"), p.gridColor.name(QColor::HexArgb));
    st.setValue(QStringLiteral("checkerboard"), p.checkerboard);
    st.setValue(QStringLiteral("canvasBorder"), p.canvasBorder);
    st.setValue(QStringLiteral("fitMargin"), p.fitMargin);
    st.setValue(QStringLiteral("useSystemFonts"), p.useSystemFonts);
    st.setValue(QStringLiteral("useCustomFonts"), p.useCustomFonts);
    st.setValue(QStringLiteral("customFontDir"), p.customFontDir);
    st.setValue(QStringLiteral("carryToolSettings"), p.carryToolSettings);
    st.setValue(QStringLiteral("pasteAtMouse"), p.pasteAtMouse);
    st.setValue(QStringLiteral("useSystemIcons"), p.useSystemIcons);
    st.setValue(QStringLiteral("useSystemFileDialogs"), p.useSystemFileDialogs);
    st.remove(QStringLiteral("usePolygonSlices"));
    st.setValue(QStringLiteral("enableWebFeatures"), p.enableWebFeatures);
    st.setValue(QStringLiteral("canvasShadow"), p.canvasShadow);
    st.setValue(QStringLiteral("snapToCanvas"), p.snapToCanvas);
    st.setValue(QStringLiteral("snapToObjects"), p.snapToObjects);
    st.setValue(QStringLiteral("recentFiles"), p.recentFiles);
    st.setValue(QStringLiteral("hdrToneMap"), p.hdrToneMap);
    st.setValue(QStringLiteral("hdrExposure"), p.hdrExposure);
    st.setValue(QStringLiteral("tabletCurve"), tablet::encodeCurve(p.tabletCurve));
    st.setValue(QStringLiteral("tabletMinSize"), p.tabletMinSize);
    st.setValue(QStringLiteral("tabletMinOpacity"), p.tabletMinOpacity);
    st.setValue(QStringLiteral("tabletEraser"), p.tabletEraser);
    st.endGroup();
}

namespace {
struct OptField {
    const char *key;
    std::function<QVariant(const AppState &)> get;
    std::function<void(AppState &, const QVariant &)> set;
};
template <class T> OptField field(const char *k, T AppState::*m) {
    return {k, [m](const AppState &a) { return QVariant::fromValue(a.*m); },
            [m](AppState &a, const QVariant &v) { if (v.isValid() && v.canConvert<T>()) a.*m = v.value<T>(); }};
}
template <class T> OptField bounded(const char *k, T AppState::*m, T lo, T hi) {
    return {k, [m](const AppState &a) { return QVariant::fromValue(a.*m); },
            [m, lo, hi](AppState &a, const QVariant &v) { if (v.isValid() && v.canConvert<T>()) a.*m = qBound(lo, v.value<T>(), hi); }};
}
const QVector<OptField> &optFields() {
    static const QVector<OptField> f = {
        // per tool
        bounded("brushSize", &AppState::brushSize, 1, 1000), bounded("brushEdge", &AppState::brushEdge, 0, 100),
        bounded("tipShape", &AppState::tipShape, 0, 1), bounded("toolIntensity", &AppState::toolIntensity, 1, 100),
        bounded("toolOpacity", &AppState::toolOpacity, 1, 100), bounded("toolBlend", &AppState::toolBlend, 0, 11),
        bounded("brushCategory", &AppState::brushCategory, 0, 12), bounded("brushTip", &AppState::brushTip, 0, 16), field("brushTexture", &AppState::brushTexture),
        bounded("brushTextureAmount", &AppState::brushTextureAmount, 0, 100), field("antialias", &AppState::antialias),
        field("pressureSize", &AppState::pressureSize), field("pressureOpacity", &AppState::pressureOpacity),
        field("pressureFlow", &AppState::pressureFlow), field("tiltAngle", &AppState::tiltAngle),
        // shared
        field("strokeWidth", &AppState::strokeWidth), field("roundness", &AppState::roundness),
        bounded("pathToolSize", &AppState::pathToolSize, 1, 1000),
        bounded("pathPrecision", &AppState::pathPrecision, 1, 10), bounded("reshapeStrength", &AppState::reshapeStrength, 1, 100),
        field("gradientKind", &AppState::gradientKind), bounded("tolerance", &AppState::tolerance, 0, 255),
        bounded("toneRange", &AppState::toneRange, 0, 2), field("stampAligned", &AppState::stampAligned),
        bounded("marqueeStyle", &AppState::marqueeStyle, 0, 2), field("marqueeRatioW", &AppState::marqueeRatioW),
        field("marqueeRatioH", &AppState::marqueeRatioH), bounded("marqueeFixedW", &AppState::marqueeFixedW, 1, 30000),
        bounded("marqueeFixedH", &AppState::marqueeFixedH, 1, 30000), bounded("marqueeEdge", &AppState::marqueeEdge, 0, 2),
        bounded("lassoEdge", &AppState::lassoEdge, 0, 2), bounded("wandEdge", &AppState::wandEdge, 0, 2),
        field("marqueeFeather", &AppState::marqueeFeather), field("lassoFeather", &AppState::lassoFeather),
        field("wandFeather", &AppState::wandFeather), bounded("healMode", &AppState::healMode, 0, 3),
        field("useEntireDocument", &AppState::useEntireDocument), field("wandSampleAllLayers", &AppState::wandSampleAllLayers),
        bounded("strokeTarget", &AppState::strokeTarget, 0, 2),
        field("contiguous", &AppState::contiguous), field("autoErase", &AppState::autoErase),
        field("preserveTransparency", &AppState::preserveTransparency), field("smudgeUseColor", &AppState::smudgeUseColor),
        field("smudgeColor", &AppState::smudgeColor), field("rcFrom", &AppState::rcFrom), field("rcTo", &AppState::rcTo),
        field("rcFromImage", &AppState::rcFromImage), bounded("rcTolerance", &AppState::rcTolerance, 0, 100),
        bounded("rcRange", &AppState::rcRange, 0, 1), bounded("reTolerance", &AppState::reTolerance, 0, 100),
        bounded("reDarken", &AppState::reDarken, 0, 100), field("rcPixelPerfect", &AppState::rcPixelPerfect), bounded("eyedropperSample", &AppState::eyedropperSample, 0, 2),
        field("fillSelectionOnly", &AppState::fillSelectionOnly), field("fillSelectedObject", &AppState::fillSelectedObject),
        bounded("fillEdge", &AppState::fillEdge, 0, 2), bounded("fillFeather", &AppState::fillFeather, 0, 250),
        bounded("polygonSides", &AppState::polygonSides, 3, 360), field("polygonStar", &AppState::polygonStar), field("pixelPerfect", &AppState::pixelPerfect),
        field("pencilTwoColor", &AppState::pencilTwoColor), field("pencilLeft", &AppState::pencilLeft), field("pencilRight", &AppState::pencilRight),
        field("polygonAutoAngle", &AppState::polygonAutoAngle), bounded("polygonAngle", &AppState::polygonAngle, 1.0, 179.0),
        field("pathPressure", &AppState::pathPressure), bounded("psMode", &AppState::psMode, 0, 1),
        field("psAffectSize", &AppState::psAffectSize), field("psAffectOpacity", &AppState::psAffectOpacity),
        bounded("psStrength", &AppState::psStrength, 1, 100), bounded("psSize", &AppState::psSize, 1, 1000), field("linkUrl", &AppState::linkUrl), field("linkAlt", &AppState::linkAlt),
        field("linkTarget", &AppState::linkTarget), field("fontFamily", &AppState::fontFamily),
        bounded("fontSize", &AppState::fontSize, 1.0, 2000.0), field("textColor", &AppState::textColor), field("bold", &AppState::bold),
        field("italic", &AppState::italic), field("underline", &AppState::underline), field("kerning", &AppState::kerning),
        field("leading", &AppState::leading), bounded("hScale", &AppState::hScale, 1.0, 1000.0), field("baselineShift", &AppState::baselineShift),
        // auto shape tool parameters, "kind:key=value,key=value;kind:..." (only the ones changed from the defaults)
        {"autoShapeParams", [](const AppState &a) {
             QStringList kinds;
             for (auto it = a.autoShapeParams.cbegin(); it != a.autoShapeParams.cend(); ++it) {
                 QStringList kv;
                 for (auto p = it.value().cbegin(); p != it.value().cend(); ++p) kv << p.key() + QLatin1Char('=') + QString::number(p.value());
                 if (!kv.isEmpty()) kinds << it.key() + QLatin1Char(':') + kv.join(QLatin1Char(','));
             }
             return QVariant(kinds.join(QLatin1Char(';')));
         },
         [](AppState &a, const QVariant &v) {
             if (!v.isValid()) return;
             a.autoShapeParams.clear();
             for (const QString &k : v.toString().split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                 const QString kind = k.section(QLatin1Char(':'), 0, 0);
                 if (!autoshape::isKind(kind)) continue;
                 const QStringList keys = autoshape::defaults(kind).keys();
                 for (const QString &kv : k.section(QLatin1Char(':'), 1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                     bool ok = false;
                     const double d = kv.section(QLatin1Char('='), 1).toDouble(&ok);
                     if (ok && keys.contains(kv.section(QLatin1Char('='), 0, 0))) a.autoShapeParams[kind][kv.section(QLatin1Char('='), 0, 0)] = d;
                 }
             }
         }},
        {"textAA", [](const AppState &a) { return QVariant(int(a.textAA)); },
         [](AppState &a, const QVariant &v) { if (v.isValid()) a.textAA = TextAA(qBound(0, v.toInt(), 4)); }},
        field("showRulers", &AppState::showRulers), field("strokeOn", &AppState::strokeOn), field("fillOn", &AppState::fillOn), field("strokeKind", &AppState::strokeKind),
        bounded("textAlign", &AppState::textAlign, 0, 3),
        bounded("textOrientation", &AppState::textOrientation, 0, 1),
        bounded("textDirection", &AppState::textDirection, 0, 1),
        bounded("paraIndent", &AppState::paraIndent, 0.0, 10000.0),
        bounded("paraSpaceBefore", &AppState::paraSpaceBefore, 0.0, 10000.0),
        bounded("paraSpaceAfter", &AppState::paraSpaceAfter, 0.0, 10000.0),
        {"gradientStops", [](const AppState &a) { return QVariant(AppState::encodeStops(a.gradientStops)); },
         [](AppState &a, const QVariant &v) { if (v.isValid()) a.gradientStops = AppState::decodeStops(v.toString()); }},
    };
    return f;
}
}

AppState::AppState() = default;

QString AppState::encodeStops(const QVector<GradStop> &s) {
    QStringList out;
    for (const GradStop &g : s) out << QStringLiteral("%1:%2").arg(g.pos).arg(g.color.name(QColor::HexArgb));
    return out.join(QLatin1Char(';'));
}

QVector<GradStop> AppState::decodeStops(const QString &s) {
    QVector<GradStop> out;
    for (const QString &part : s.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList kv = part.split(QLatin1Char(':'));
        if (kv.size() != 2) continue;
        bool ok = false;
        const double pos = kv[0].toDouble(&ok);
        const QColor c(kv[1]);
        if (ok && c.isValid()) out << GradStop{qBound(0.0, pos, 1.0), c};
    }
    return out.size() >= 2 ? out : QVector<GradStop>();
}

QStringList AppState::perToolKeys() {
    return {QStringLiteral("brushSize"), QStringLiteral("brushEdge"), QStringLiteral("tipShape"), QStringLiteral("toolIntensity"),
            QStringLiteral("toolOpacity"), QStringLiteral("toolBlend"), QStringLiteral("brushCategory"), QStringLiteral("brushTip"), QStringLiteral("brushTexture"),
            QStringLiteral("brushTextureAmount"), QStringLiteral("antialias"), QStringLiteral("pressureSize"),
            QStringLiteral("pressureOpacity"), QStringLiteral("pressureFlow"), QStringLiteral("tiltAngle")};
}

bool AppState::hasToolSlot(Tool t) {
    switch (t) {
    case Tool::Brush: case Tool::Pencil: case Tool::Eraser: case Tool::Blur: case Tool::Sharpen: case Tool::Dodge: case Tool::Burn:
    case Tool::Smudge: case Tool::RubberStamp: case Tool::Healing: case Tool::SpotHealing: case Tool::PaintBucket: case Tool::Gradient:
    case Tool::ReplaceColor: case Tool::RedEye:
        return true;
    default: return false;
    }
}

// Fireworks-like starting values for each painting tool
QVariantMap AppState::defaultSlot(Tool t) {
    QVariantMap m{{QStringLiteral("brushSize"), 20}, {QStringLiteral("brushEdge"), 50}, {QStringLiteral("tipShape"), 0},
                  {QStringLiteral("toolIntensity"), 50}, {QStringLiteral("toolOpacity"), 100}, {QStringLiteral("toolBlend"), 0},
                  {QStringLiteral("brushCategory"), 0}, {QStringLiteral("brushTip"), 0}, {QStringLiteral("brushTexture"), QString()},
                  {QStringLiteral("brushTextureAmount"), 0}, {QStringLiteral("antialias"), true},
                  {QStringLiteral("pressureSize"), true}, {QStringLiteral("pressureOpacity"), false},
                  {QStringLiteral("pressureFlow"), false}, {QStringLiteral("tiltAngle"), true}};
    switch (t) {
    case Tool::Brush: m[QStringLiteral("brushSize")] = 6; m[QStringLiteral("brushEdge")] = 0; m[QStringLiteral("brushTextureAmount")] = 50; break;
    case Tool::Pencil: m[QStringLiteral("brushSize")] = 1; m[QStringLiteral("brushEdge")] = 0; m[QStringLiteral("antialias")] = false; break;
    case Tool::Eraser: m[QStringLiteral("brushSize")] = 16; m[QStringLiteral("brushEdge")] = 0; break;
    case Tool::PaintBucket: case Tool::Gradient: m[QStringLiteral("brushTextureAmount")] = 50; break;
    case Tool::RedEye: m[QStringLiteral("brushSize")] = 24; m[QStringLiteral("brushEdge")] = 40; break;
    default: break;
    }
    return m;
}

QVariantMap AppState::optionValues() const {
    QVariantMap m;
    for (const OptField &f : optFields()) m.insert(QString::fromLatin1(f.key), f.get(*this));
    // the other tools' own copies, so a change to any tool's option shows up
    for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
        for (auto k = it.value().begin(); k != it.value().end(); ++k)
            m.insert(toolName(Tool(it.key())) + QLatin1Char('/') + k.key(), k.value());
    return m;
}

void AppState::setOptionValues(const QVariantMap &m) {
    for (const OptField &f : optFields()) {
        const auto it = m.find(QString::fromLatin1(f.key));
        if (it != m.end()) f.set(*this, it.value());
    }
}

void AppState::loadToolOptions() {
    // 0.3.7: tool options are per-document (Document::toolEnv). Drop any legacy QSettings copy
    // so old installs do not keep resurrecting global brush sizes.
    QSettings st;
    st.beginGroup(QStringLiteral("toolOptions"));
    st.remove(QString());
    st.endGroup();
}

static QVariantMap variantMapFromJson(const QJsonObject &o) {
    QVariantMap m;
    for (auto it = o.begin(); it != o.end(); ++it) m.insert(it.key(), it.value().toVariant());
    return m;
}
static QJsonObject jsonFromVariantMap(const QVariantMap &m) {
    QJsonObject o;
    for (auto it = m.begin(); it != m.end(); ++it) o.insert(it.key(), QJsonValue::fromVariant(it.value()));
    return o;
}

QJsonObject AppState::defaultEnvironment() {
    AppState fresh; // default-constructed tool / colour / option state
    // Build slots for every painting tool at Fireworks-like defaults
    for (int i = 0; i <= int(kLastTool); ++i) {
        const Tool t = Tool(i);
        if (hasToolSlot(t)) fresh.m_slots.insert(i, defaultSlot(t));
    }
    // Active tool's slot is live on the fields, not in m_slots
    if (hasToolSlot(fresh.m_tool)) {
        fresh.setOptionValues(defaultSlot(fresh.m_tool));
        fresh.m_slots.remove(int(fresh.m_tool));
    }
    return fresh.captureEnvironment();
}

QJsonObject AppState::captureEnvironment() const {
    QJsonObject root;
    root.insert(QStringLiteral("tool"), int(m_tool));
    root.insert(QStringLiteral("stroke"), stroke.name(QColor::HexArgb));
    root.insert(QStringLiteral("fill"), fill.name(QColor::HexArgb));
    QVariantMap opts = optionValues();
    opts.remove(QStringLiteral("showRulers")); // application-wide
    // drop per-tool "Name/key" mirrors from optionValues — slots stored separately
    for (auto it = opts.begin(); it != opts.end(); ) {
        if (it.key().contains(QLatin1Char('/'))) it = opts.erase(it);
        else ++it;
    }
    root.insert(QStringLiteral("options"), jsonFromVariantMap(opts));
    QJsonObject slotMap;
    const QStringList own = perToolKeys();
    // include the active tool's live values as a slot too, so reloading is order-independent
    if (hasToolSlot(m_tool)) {
        QVariantMap cur;
        for (const QString &k : own) cur.insert(k, opts.value(k));
        slotMap.insert(toolName(m_tool).remove(QLatin1Char(' ')), jsonFromVariantMap(cur));
    }
    for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
        slotMap.insert(toolName(Tool(it.key())).remove(QLatin1Char(' ')), jsonFromVariantMap(it.value()));
    root.insert(QStringLiteral("slots"), slotMap);
    return root;
}

void AppState::applyEnvironment(const QJsonObject &env) {
    const bool keepRulers = showRulers;
    if (env.isEmpty()) {
        applyEnvironment(defaultEnvironment());
        showRulers = keepRulers;
        emit toolChanged(m_tool);
        emit colorsChanged();
        emit optionsChanged();
        return;
    }
    m_slots.clear();
    const QJsonObject slotMap = env.value(QStringLiteral("slots")).toObject();
    for (int i = 0; i <= int(kLastTool); ++i) {
        const Tool t = Tool(i);
        if (!hasToolSlot(t)) continue;
        const QString key = toolName(t).remove(QLatin1Char(' '));
        if (slotMap.contains(key)) {
            QVariantMap slot = defaultSlot(t);
            const QVariantMap got = variantMapFromJson(slotMap.value(key).toObject());
            for (auto it = got.begin(); it != got.end(); ++it)
                if (slot.contains(it.key())) slot.insert(it.key(), it.value());
            m_slots.insert(i, slot);
        } else {
            m_slots.insert(i, defaultSlot(t));
        }
    }
    const QVariantMap opts = variantMapFromJson(env.value(QStringLiteral("options")).toObject());
    setOptionValues(opts);
    const QString sc = env.value(QStringLiteral("stroke")).toString();
    const QString fc = env.value(QStringLiteral("fill")).toString();
    if (QColor c(sc); c.isValid()) stroke = c;
    if (QColor c(fc); c.isValid()) fill = c;
    Tool t = Tool(env.value(QStringLiteral("tool")).toInt(int(Tool::Pointer)));
    if (int(t) < 0 || int(t) > int(kLastTool)) t = Tool::Pointer;
    if ((toolIsHotspot(t) || toolIsSlice(t)) && !prefs.enableWebFeatures) t = Tool::Pointer;
    // Install the active tool's slot into the live fields
    if (hasToolSlot(t) && m_slots.contains(int(t))) {
        setOptionValues(m_slots.value(int(t)));
        m_slots.remove(int(t));
    }
    m_tool = t;
    showRulers = keepRulers;
    emit toolChanged(m_tool);
    emit colorsChanged();
    emit optionsChanged();
}

void AppState::saveCarriedEnvironment() const {
    if (!prefs.carryToolSettings) return;
    QSettings st;
    const QJsonObject env = captureEnvironment();
    st.setValue(QStringLiteral("carriedToolEnv"),
                QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
}

bool AppState::loadCarriedEnvironment() {
    if (!prefs.carryToolSettings) return false;
    QSettings st;
    const QByteArray js = st.value(QStringLiteral("carriedToolEnv")).toString().toUtf8();
    if (js.isEmpty()) return false;
    const QJsonObject env = QJsonDocument::fromJson(js).object();
    if (env.isEmpty()) return false;
    applyEnvironment(env);
    return true;
}

void AppState::setTool(Tool t) {
    if ((toolIsHotspot(t) || toolIsSlice(t)) && !prefs.enableWebFeatures) t = Tool::Pointer;
    if (t == m_tool) return;
    // painting tools keep their own size, edge, opacity...: stash the old tool's, bring in the new one's
    if (hasToolSlot(m_tool)) {
        QVariantMap cur;
        const QVariantMap all = optionValues();
        for (const QString &k : perToolKeys()) cur.insert(k, all.value(k));
        m_slots.insert(int(m_tool), cur);
    }
    if (hasToolSlot(t)) {
        setOptionValues(m_slots.contains(int(t)) ? m_slots.value(int(t)) : defaultSlot(t));
        m_slots.remove(int(t));
    }
    m_tool = t;
    emit toolChanged(t);
}

QString toolName(Tool t) {
    switch (t) {
    case Tool::Pointer: return QStringLiteral("Pointer");
    case Tool::Subselect: return QStringLiteral("Subselection");
    case Tool::Scale: return QStringLiteral("Scale");
    case Tool::Skew: return QStringLiteral("Skew");
    case Tool::Distort: return QStringLiteral("Distort");
    case Tool::Perspective: return QStringLiteral("Perspective");
    case Tool::Crop: return QStringLiteral("Crop");
    case Tool::Marquee: return QStringLiteral("Marquee");
    case Tool::OvalMarquee: return QStringLiteral("Oval Marquee");
    case Tool::Lasso: return QStringLiteral("Lasso");
    case Tool::PolyLasso: return QStringLiteral("Polygon Lasso");
    case Tool::MagicWand: return QStringLiteral("Magic Wand");
    case Tool::Brush: return QStringLiteral("Brush");
    case Tool::Pencil: return QStringLiteral("Pencil");
    case Tool::Eraser: return QStringLiteral("Eraser");
    case Tool::Blur: return QStringLiteral("Blur");
    case Tool::Sharpen: return QStringLiteral("Sharpen");
    case Tool::Dodge: return QStringLiteral("Dodge");
    case Tool::Burn: return QStringLiteral("Burn");
    case Tool::Smudge: return QStringLiteral("Smudge");
    case Tool::RubberStamp: return QStringLiteral("Rubber Stamp");
    case Tool::Healing: return QStringLiteral("Healing Brush");
    case Tool::SpotHealing: return QStringLiteral("Spot Healing Brush");
    case Tool::Eyedropper: return QStringLiteral("Eyedropper");
    case Tool::PaintBucket: return QStringLiteral("Paint Bucket");
    case Tool::Gradient: return QStringLiteral("Gradient");
    case Tool::Line: return QStringLiteral("Line");
    case Tool::Pen: return QStringLiteral("Pen");
    case Tool::Rect: return QStringLiteral("Rectangle");
    case Tool::Ellipse: return QStringLiteral("Ellipse");
    case Tool::Polygon: return QStringLiteral("Polygon");
    case Tool::Text: return QStringLiteral("Text");
    case Tool::Knife: return QStringLiteral("Knife");
    case Tool::Freeform: return QStringLiteral("Freeform");
    case Tool::ReshapeArea: return QStringLiteral("Reshape Area");
    case Tool::Slice: return QStringLiteral("Slice");
    case Tool::PolySlice: return QStringLiteral("Polygon Slice");
    case Tool::SelectBehind: return QStringLiteral("Select Behind");
    case Tool::VectorPath: return QStringLiteral("Vector Path");
    case Tool::RedrawPath: return QStringLiteral("Redraw Path");
    case Tool::Hotspot: return QStringLiteral("Rectangle Hotspot");
    case Tool::CircleHotspot: return QStringLiteral("Circle Hotspot");
    case Tool::PolyHotspot: return QStringLiteral("Polygon Hotspot");
    case Tool::ContentAware: return QStringLiteral("Content-Aware Remove");
    case Tool::ReplaceColor: return QStringLiteral("Replace Color");
    case Tool::ExportArea: return QStringLiteral("Export Area");
    case Tool::RedEye: return QStringLiteral("Red Eye Removal");
    case Tool::PathScrubber: return QStringLiteral("Path Scrubber");
    case Tool::Hand: return QStringLiteral("Hand");
    case Tool::Zoom: return QStringLiteral("Zoom");
    default:
        if (toolIsAutoShape(t)) return autoshape::label(toolAutoShapeKind(t));
    }
    return {};
}

QString toolAutoShapeKind(Tool t) {
    if (!toolIsAutoShape(t)) return {};
    // Heart is appended after RedrawPath so saved tool indices stay stable; it is not
    // in the contiguous AsRoundRect..AsSpeech block.
    if (t == Tool::AsHeart) return QStringLiteral("heart");
    const QStringList k = autoshape::kinds();
    const int i = int(t) - int(Tool::AsRoundRect);
    if (i < 0 || i >= k.size()) return {};
    const QString kind = k[i];
    return kind == QLatin1String("heart") ? QString() : kind;
}

Tool autoShapeTool(const QString &kind) {
    if (kind == QLatin1String("heart")) return Tool::AsHeart;
    const int i = autoshape::kinds().indexOf(kind);
    if (i < 0 || kind == QLatin1String("heart")) return Tool::Pointer;
    return Tool(int(Tool::AsRoundRect) + i);
}

QMap<QString, double> AppState::autoShapeParamsFor(const QString &kind) const {
    QMap<QString, double> m = autoshape::defaults(kind);
    const auto own = autoShapeParams.value(kind);
    for (auto it = own.cbegin(); it != own.cend(); ++it) m[it.key()] = it.value();
    return m;
}


QString toolActionId(Tool t) {
    if (toolIsAutoShape(t))
        return QStringLiteral("tool.autoshape.") + toolAutoShapeKind(t);
    const QString n = toolName(t);
    return n.isEmpty() ? QString() : QStringLiteral("tool.") + n;
}

Tool toolFromActionId(const QString &id) {
    if (id.startsWith(QLatin1String("tool.autoshape.")))
        return autoShapeTool(id.mid(QStringLiteral("tool.autoshape.").size()));
    if (!id.startsWith(QLatin1String("tool."))) return Tool::Pointer;
    const QString name = id.mid(5);
    for (int i = 0; i <= int(kLastTool); ++i) {
        Tool t = Tool(i);
        if (toolIsAutoShape(t)) continue;
        if (toolName(t) == name) return t;
    }
    return Tool::Pointer;
}

QString toolShortcut(Tool t) {
    switch (t) {
    case Tool::Pointer: case Tool::SelectBehind: return QStringLiteral("V");
    case Tool::Subselect: return QStringLiteral("A");
    case Tool::Scale: case Tool::Skew: case Tool::Distort: case Tool::Perspective: return QStringLiteral("Q");
    case Tool::Crop: case Tool::ExportArea: return QStringLiteral("C");
    case Tool::Marquee: case Tool::OvalMarquee: return QStringLiteral("M");
    case Tool::Lasso: case Tool::PolyLasso: return QStringLiteral("L");
    case Tool::MagicWand: return QStringLiteral("W");
    case Tool::Brush: return QStringLiteral("B");
    case Tool::Pencil: case Tool::Knife: return QStringLiteral("Y");
    case Tool::Eraser: return QStringLiteral("E");
    case Tool::Blur: case Tool::Sharpen: case Tool::Dodge: case Tool::Burn: case Tool::Smudge: return QStringLiteral("R");
    case Tool::RubberStamp: case Tool::Healing: case Tool::SpotHealing: case Tool::ContentAware: case Tool::ReplaceColor: case Tool::RedEye: return QStringLiteral("S");
    case Tool::Eyedropper: return QStringLiteral("I");
    case Tool::PaintBucket: case Tool::Gradient: return QStringLiteral("G");
    case Tool::Line: return QStringLiteral("N");
    case Tool::Pen: case Tool::VectorPath: case Tool::RedrawPath: return QStringLiteral("P");
    case Tool::Rect: case Tool::Ellipse: case Tool::Polygon: return QStringLiteral("U");
    case Tool::Text: return QStringLiteral("T");
    case Tool::Freeform: case Tool::ReshapeArea: case Tool::PathScrubber: return QStringLiteral("O");
    case Tool::Slice: case Tool::PolySlice: return QStringLiteral("K");
    case Tool::Hotspot: case Tool::CircleHotspot: case Tool::PolyHotspot: return QStringLiteral("J");
    case Tool::Hand: return QStringLiteral("H");
    case Tool::Zoom: return QStringLiteral("Z");
    default:
        // Auto Shapes: picked from the shape flyout or the Auto Shapes panel; U cycles only Rectangle/Ellipse/Polygon
        break;
    }
    return {};
}

bool toolIsStub(Tool t) {
    Q_UNUSED(t); // every tool is implemented now (the Gradient tool was the last one)
    return false;
}

QList<Tool> toolGroup(Tool t) {
    static const QList<QList<Tool>> groups = {
        {Tool::Pointer, Tool::SelectBehind},
        {Tool::Pen, Tool::VectorPath, Tool::RedrawPath},
        {Tool::Scale, Tool::Skew, Tool::Distort, Tool::Perspective},
        {Tool::Crop, Tool::ExportArea},
        {Tool::Marquee, Tool::OvalMarquee},
        {Tool::Lasso, Tool::PolyLasso},
        {Tool::Blur, Tool::Sharpen, Tool::Dodge, Tool::Burn, Tool::Smudge},
        {Tool::RubberStamp, Tool::ReplaceColor, Tool::RedEye, Tool::Healing, Tool::SpotHealing, Tool::ContentAware},
        {Tool::PaintBucket, Tool::Gradient},
        {Tool::Rect, Tool::Ellipse, Tool::Polygon, Tool::AsRoundRect, Tool::AsChamfer, Tool::AsBevel, Tool::AsPolygon, Tool::AsStar,
         Tool::AsSmartPolygon, Tool::AsArc, Tool::AsDoughnut, Tool::AsSpiral, Tool::AsArrow, Tool::AsArrowLine, Tool::AsConnector,
         Tool::AsGear, Tool::AsFrame, Tool::AsLShape, Tool::AsSpeech, Tool::AsHeart},
        {Tool::Freeform, Tool::ReshapeArea, Tool::PathScrubber},
        {Tool::Hotspot, Tool::CircleHotspot, Tool::PolyHotspot},
        {Tool::Slice, Tool::PolySlice},
    };
    for (const auto &g : groups)
        if (g.contains(t)) return g;
    return {t};
}

bool toolIsHotspot(Tool t) { return t == Tool::Hotspot || t == Tool::CircleHotspot || t == Tool::PolyHotspot; }
bool toolIsSlice(Tool t) { return t == Tool::Slice || t == Tool::PolySlice; }
bool toolAvailable(Tool t) {
    if (toolIsHotspot(t) || toolIsSlice(t))
        return appState().prefs.enableWebFeatures;
    return true;
}

bool webFeaturesEnabled() { return appState().prefs.enableWebFeatures; }

bool isWebFeatureAction(const QString &id)
{
    if (id == QLatin1String("file.exporthtml") || id == QLatin1String("file.exportimagemap")
        || id == QLatin1String("file.exportslices") || id == QLatin1String("edit.inserthotspot")
        || id == QLatin1String("edit.insertslice"))
        return true;
    const Tool t = toolFromActionId(id);
    return id.startsWith(QLatin1String("tool.")) && (toolIsHotspot(t) || toolIsSlice(t));
}

bool toolIsBitmap(Tool t) {
    switch (t) {
    case Tool::Marquee: case Tool::OvalMarquee: case Tool::Lasso: case Tool::PolyLasso: case Tool::MagicWand: case Tool::Brush: case Tool::Pencil:
    case Tool::Eraser: case Tool::Blur: case Tool::Sharpen: case Tool::Dodge: case Tool::Burn: case Tool::Smudge:
    case Tool::RubberStamp: case Tool::Healing: case Tool::SpotHealing: case Tool::ContentAware: case Tool::ReplaceColor: case Tool::RedEye: case Tool::Eyedropper: case Tool::PaintBucket:
        return true;
    default: return false;
    }
}

void drawCanvasShadow(QPainter &p, const QRectF &r) {
    // a 4px offset shadow that fades out (only the part outside the document shows)
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 4; ++i) p.fillRect(r.translated(2, 2).adjusted(-i, -i, i + 1, i + 1), QColor(0, 0, 0, 22 - i * 4));
    p.restore();
}

namespace {
QVector<int> g_customFontIds;
QStringList g_customFontFamilies;
QFont g_savedAppFont;
bool g_appFontOverridden = false;

QStringList familiesFromAppFonts() {
    QStringList out;
    for (int id : g_customFontIds)
        out += QFontDatabase::applicationFontFamilies(id);
    out.removeDuplicates();
    out.sort(Qt::CaseInsensitive);
    return out;
}
}

QString AppState::fallbackUiFontFamily() {
    // A generic sans so the interface still has a face when system fonts are not listed.
    QFont f(QStringLiteral("Sans Serif"));
    f.setStyleHint(QFont::SansSerif);
    return QFontInfo(f).family();
}

QStringList AppState::fontSourceFamilies() const {
    if (prefs.useSystemFonts)
        return QFontDatabase::families();
    if (prefs.useCustomFonts)
        return g_customFontFamilies;
    return {};
}

void AppState::applyUiFontForSources() {
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app) return;
    if (!prefs.useSystemFonts) {
        const QString cur = app->font().family();
        const QString resolved = QFontInfo(QFont(cur)).family();
        const bool ok = g_customFontFamilies.contains(cur, Qt::CaseInsensitive)
                     || g_customFontFamilies.contains(resolved, Qt::CaseInsensitive);
        if (!ok) {
            if (!g_appFontOverridden) {
                g_savedAppFont = app->font();
                g_appFontOverridden = true;
            }
            QFont f(fallbackUiFontFamily());
            f.setStyleHint(QFont::SansSerif);
            if (app->font().pixelSize() > 0) f.setPixelSize(app->font().pixelSize());
            else if (app->font().pointSizeF() > 0) f.setPointSizeF(app->font().pointSizeF());
            app->setFont(f);
        }
    } else if (g_appFontOverridden) {
        app->setFont(g_savedAppFont);
        g_appFontOverridden = false;
    }
}

void AppState::applyFontSources() {
    // remove previously added application fonts, then re-add from the custom folder when enabled
    for (int id : g_customFontIds) QFontDatabase::removeApplicationFont(id);
    g_customFontIds.clear();
    g_customFontFamilies.clear();
    const bool custom = prefs.useCustomFonts;
    // Only force a source when both would be off. Custom-only with an empty folder is allowed.
    if (!prefs.useSystemFonts && !custom) prefs.useSystemFonts = true;
    if (custom && !prefs.customFontDir.isEmpty()) {
        QDirIterator it(prefs.customFontDir, {QStringLiteral("*.ttf"), QStringLiteral("*.otf"), QStringLiteral("*.ttc"), QStringLiteral("*.pfb"),
                                              QStringLiteral("*.TTF"), QStringLiteral("*.OTF"), QStringLiteral("*.TTC"), QStringLiteral("*.PFB")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const int id = QFontDatabase::addApplicationFont(it.next());
            if (id >= 0) g_customFontIds.append(id);
        }
    }
    g_customFontFamilies = familiesFromAppFonts();
    // Qt cannot hide system fonts from QFontDatabase, so UI lists use fontSourceFamilies()
    // and only show application (custom-folder) fonts when useSystemFonts is off.
}
