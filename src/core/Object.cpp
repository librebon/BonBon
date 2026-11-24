#include "Object.h"
#include <QImage>
#include <QRgba64>
#include "CorePrefs.h"
#include "Paths.h"
#include "Fills.h"
#include "AutoShape.h"
#include <QFontMetricsF>
#include <QPainterPathStroker>
#include <QtMath>
#include <QSet>
#include <cmath>

static const struct { BlendMode m; const char *key; const char *label; QPainter::CompositionMode cm; } kBlend[] = {
    {BlendMode::Normal, "normal", "Normal", QPainter::CompositionMode_SourceOver},
    {BlendMode::Multiply, "multiply", "Multiply", QPainter::CompositionMode_Multiply},
    {BlendMode::Screen, "screen", "Screen", QPainter::CompositionMode_Screen},
    {BlendMode::Overlay, "overlay", "Overlay", QPainter::CompositionMode_Overlay},
    {BlendMode::Darken, "darken", "Darken", QPainter::CompositionMode_Darken},
    {BlendMode::Lighten, "lighten", "Lighten", QPainter::CompositionMode_Lighten},
    {BlendMode::Difference, "difference", "Difference", QPainter::CompositionMode_Difference},
    {BlendMode::ColorDodge, "colorDodge", "Color Dodge", QPainter::CompositionMode_ColorDodge},
    {BlendMode::ColorBurn, "colorBurn", "Color Burn", QPainter::CompositionMode_ColorBurn},
    {BlendMode::HardLight, "hardLight", "Hard Light", QPainter::CompositionMode_HardLight},
    {BlendMode::SoftLight, "softLight", "Soft Light", QPainter::CompositionMode_SoftLight},
    {BlendMode::Exclusion, "exclusion", "Exclusion", QPainter::CompositionMode_Exclusion},
    {BlendMode::Hue, "hue", "Hue", QPainter::CompositionMode_SourceOver},
    {BlendMode::Saturation, "saturation", "Saturation", QPainter::CompositionMode_SourceOver},
    {BlendMode::Color, "color", "Color", QPainter::CompositionMode_SourceOver},
    {BlendMode::Luminosity, "luminosity", "Luminosity", QPainter::CompositionMode_SourceOver},
    {BlendMode::Invert, "invert", "Invert", QPainter::CompositionMode_SourceOver},
    {BlendMode::Tint, "tint", "Tint", QPainter::CompositionMode_SourceOver},
    {BlendMode::Erase, "erase", "Erase", QPainter::CompositionMode_DestinationOut},
};

QStringList blendModeNames() {
    QStringList l;
    for (auto &b : kBlend) l << QString::fromLatin1(b.label);
    return l;
}
QString blendModeKey(BlendMode m) {
    for (auto &b : kBlend) if (b.m == m) return QString::fromLatin1(b.key);
    return QStringLiteral("normal");
}
BlendMode blendModeFromKey(const QString &k) {
    for (auto &b : kBlend) if (k == QLatin1String(b.key)) return b.m;
    return BlendMode::Normal;
}
QPainter::CompositionMode toComposition(BlendMode m) {
    for (auto &b : kBlend) if (b.m == m) return b.cm;
    return QPainter::CompositionMode_SourceOver;
}


bool blendIsCustom(BlendMode m)
{
    switch (m) {
    case BlendMode::Hue: case BlendMode::Saturation: case BlendMode::Color:
    case BlendMode::Luminosity: case BlendMode::Invert: case BlendMode::Tint:
        return true;
    default:
        return false;
    }
}

namespace {
struct RGB { double r, g, b; };
double lum(RGB c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; }
double sat(RGB c) { return qMax(c.r, qMax(c.g, c.b)) - qMin(c.r, qMin(c.g, c.b)); }
RGB clipColor(RGB c)
{
    const double l = lum(c);
    const double mn = qMin(c.r, qMin(c.g, c.b));
    const double mx = qMax(c.r, qMax(c.g, c.b));
    if (mn < 0) {
        const double d = l - mn;
        if (d > 1e-12) { c.r = l + (c.r - l) * l / d; c.g = l + (c.g - l) * l / d; c.b = l + (c.b - l) * l / d; }
        else c = {l, l, l};
    }
    if (mx > 1) {
        const double d = mx - l;
        if (d > 1e-12) { c.r = l + (c.r - l) * (1 - l) / d; c.g = l + (c.g - l) * (1 - l) / d; c.b = l + (c.b - l) * (1 - l) / d; }
        else c = {l, l, l};
    }
    return {qBound(0.0, c.r, 1.0), qBound(0.0, c.g, 1.0), qBound(0.0, c.b, 1.0)};
}
RGB setLum(RGB c, double l)
{
    const double d = l - lum(c);
    return clipColor({c.r + d, c.g + d, c.b + d});
}
RGB setSat(RGB c, double s)
{
    int i0 = 0, i1 = 1, i2 = 2;
    double v[3] = {c.r, c.g, c.b};
    if (v[i0] > v[i1]) qSwap(i0, i1);
    if (v[i1] > v[i2]) qSwap(i1, i2);
    if (v[i0] > v[i1]) qSwap(i0, i1);
    const double mn = v[i0], mx = v[i2];
    if (mx - mn < 1e-12) { v[i0] = v[i1] = v[i2] = 0; }
    else {
        v[i1] = (v[i1] - mn) * s / (mx - mn);
        v[i2] = s;
        v[i0] = 0;
    }
    return {v[0], v[1], v[2]};
}
RGB unpremul(QRgb px)
{
    const int a = qAlpha(px);
    if (a <= 0) return {0, 0, 0};
    const double ia = 255.0 / a;
    return {qRed(px) * ia / 255.0, qGreen(px) * ia / 255.0, qBlue(px) * ia / 255.0};
}
RGB mixRgb(RGB a, RGB b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

RGB applyCustom(BlendMode m, RGB d, RGB s)
{
    switch (m) {
    case BlendMode::Hue:        return setLum(setSat(s, sat(d)), lum(d));
    case BlendMode::Saturation: return setLum(setSat(d, sat(s)), lum(d));
    case BlendMode::Color:      return setLum(s, lum(d));
    case BlendMode::Luminosity: return setLum(d, lum(s));
    case BlendMode::Invert:     return {1 - d.r, 1 - d.g, 1 - d.b};
    case BlendMode::Tint:
        // Fireworks Tint: colourise with the blend colour, keep base luminosity.
        // Unlike Color, a grey source still tints (uses the source RGB itself).
        return setLum(mixRgb(d, s, 0.85), lum(d));
    default: return s;
    }
}
} // namespace

void blitBlend(QImage &dest, const QImage &src, const QPoint &pos, BlendMode m, qreal opacity, const QRect &clip)
{
    opacity = qBound(0.0, opacity, 1.0);
    if (opacity <= 0 || src.isNull() || dest.isNull()) return;
    QRect limit = dest.rect();
    if (!clip.isNull()) limit &= clip;
    if (limit.isEmpty()) return;
    if (!blendIsCustom(m)) {
        QPainter p(&dest);
        p.setClipRect(limit);
        p.setOpacity(opacity);
        p.setCompositionMode(toComposition(m));
        p.drawImage(pos, src);
        return;
    }
    const bool src16 = src.format() == QImage::Format_RGBA64_Premultiplied
                    || src.format() == QImage::Format_RGBA64;
    const bool dst16 = dest.format() == QImage::Format_RGBA64_Premultiplied
                    || dest.format() == QImage::Format_RGBA64;
    const QImage::Format workFmt = (src16 || dst16)
        ? QImage::Format_RGBA64_Premultiplied
        : QImage::Format_ARGB32_Premultiplied;
    if (dest.format() != workFmt) {
        const QRect dr = QRect(pos, src.size()).intersected(limit);
        if (dr.isEmpty()) return;
        QImage patch = dest.copy(dr).convertToFormat(workFmt);
        blitBlend(patch, src, pos - dr.topLeft(), m, opacity, QRect());
        QPainter p(&dest);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(dr.topLeft(), patch.convertToFormat(dest.format()));
        return;
    }
    const QImage s = src.convertToFormat(workFmt);
    const QRect dr = QRect(pos, s.size()).intersected(limit);
    if (dr.isEmpty()) return;
    auto unpremul64 = [](QRgba64 px) -> RGB {
        const int a = px.alpha();
        if (a <= 0) return {0, 0, 0};
        const double ia = 65535.0 / a;
        return {px.red() * ia / 65535.0, px.green() * ia / 65535.0, px.blue() * ia / 65535.0};
    };
    if (workFmt == QImage::Format_RGBA64_Premultiplied) {
        for (int y = dr.top(); y <= dr.bottom(); ++y) {
            auto *dline = reinterpret_cast<QRgba64 *>(dest.scanLine(y));
            const auto *sline = reinterpret_cast<const QRgba64 *>(s.constScanLine(y - pos.y()));
            for (int x = dr.left(); x <= dr.right(); ++x) {
                const QRgba64 sp = sline[x - pos.x()];
                const int sa = sp.alpha();
                if (sa <= 0) continue;
                const double cov = (sa / 65535.0) * opacity;
                const QRgba64 dp = dline[x];
                const RGB D = unpremul64(dp), S = unpremul64(sp);
                const RGB R = applyCustom(m, D, S);
                const double da = dp.alpha() / 65535.0;
                const double outA = cov + da * (1.0 - cov);
                const RGB out = mixRgb(D, R, cov / qMax(outA, 1e-6));
                const quint16 oa = quint16(qBound(0, qRound(outA * 65535), 65535));
                dline[x] = qPremultiply(qRgba64(
                    quint16(qBound(0, qRound(out.r * 65535), 65535)),
                    quint16(qBound(0, qRound(out.g * 65535), 65535)),
                    quint16(qBound(0, qRound(out.b * 65535), 65535)), oa));
            }
        }
        return;
    }
    for (int y = dr.top(); y <= dr.bottom(); ++y) {
        QRgb *dline = reinterpret_cast<QRgb *>(dest.scanLine(y));
        const QRgb *sline = reinterpret_cast<const QRgb *>(s.constScanLine(y - pos.y()));
        for (int x = dr.left(); x <= dr.right(); ++x) {
            const QRgb sp = sline[x - pos.x()];
            const int sa = qAlpha(sp);
            if (sa <= 0) continue;
            const double cov = (sa / 255.0) * opacity;
            const QRgb dp = dline[x];
            const RGB D = unpremul(dp), S = unpremul(sp);
            const RGB R = applyCustom(m, D, S);
            const int da = qAlpha(dp);
            const double outA = cov + (da / 255.0) * (1.0 - cov);
            const RGB out = mixRgb(D, R, cov / qMax(outA, 1e-6));
            const int oa = qBound(0, qRound(outA * 255), 255);
            dline[x] = qPremultiply(qRgba(
                qBound(0, qRound(out.r * 255), 255),
                qBound(0, qRound(out.g * 255), 255),
                qBound(0, qRound(out.b * 255), 255), oa));
        }
    }
}

void drawBlended(QPainter &p, const QImage &src, const QRect &srcArea, BlendMode m, qreal opacity)
{
    if (src.isNull() || srcArea.isEmpty() || opacity <= 0) return;
    if (!blendIsCustom(m)) {
        p.save();
        p.setOpacity(p.opacity() * opacity);
        p.setCompositionMode(toComposition(m));
        p.drawImage(srcArea.topLeft(), src);
        p.restore();
        return;
    }
    auto *dev = dynamic_cast<QImage *>(p.device());
    if (!dev) {
        p.save();
        p.setOpacity(p.opacity() * opacity);
        p.drawImage(srcArea.topLeft(), src);
        p.restore();
        return;
    }
    const QTransform world = p.worldTransform();
    const QPainter::RenderHints hints = p.renderHints();
    const qreal pop = p.opacity();
    const bool hasClip = p.hasClipping();
    const QPainterPath clipPath = p.clipPath();
    const QRect clipBound = hasClip ? p.clipBoundingRect().toAlignedRect() : dev->rect();

    QImage devSrc = src;
    QPoint destPos = srcArea.topLeft();
    if (world.isIdentity()) {
        // destPos already in source space
    } else {
        const QRect adr = world.mapRect(QRectF(srcArea)).toAlignedRect();
        if (adr.isEmpty()) return;
        devSrc = QImage(adr.size(), QImage::Format_ARGB32_Premultiplied);
        devSrc.fill(0);
        QPainter tp(&devSrc);
        tp.setRenderHints(hints);
        tp.setWorldTransform(world * QTransform::fromTranslate(-adr.left(), -adr.top()));
        tp.drawImage(srcArea.topLeft(), src);
        destPos = adr.topLeft();
    }
    const QRect dr = QRect(destPos, devSrc.size()).intersected(clipBound).intersected(dev->rect());
    if (dr.isEmpty()) return;
    // Offscreen layer: blend against a copy of the dest pixels, then composite back
    // with Source so the caller's painter (save stack, clip, transform) is left alone.
    QImage layer = dev->copy(dr);
    blitBlend(layer, devSrc, destPos - dr.topLeft(), m, opacity * pop, QRect());
    p.save();
    p.resetTransform();
    p.setOpacity(1);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.setClipping(false);
    p.drawImage(dr.topLeft(), layer);
    p.restore();
}


LiveEffect LiveEffect::make(EffectType t) {
    LiveEffect e;
    e.type = t;
    switch (t) {
    case EffectType::DropShadow: e.distance = 7; e.softness = 4; e.opacity = 65; e.color = Qt::black; break;
    case EffectType::InnerShadow: e.distance = 5; e.softness = 4; e.opacity = 65; e.color = Qt::black; break;
    case EffectType::OuterGlow: e.width = 4; e.softness = 4; e.opacity = 75; e.color = QColor(0xFF, 0xCC, 0x00); break;
    case EffectType::InnerBevel: e.width = 6; e.softness = 3; e.opacity = 75; e.color = Qt::white; e.angle = 135; break;
    case EffectType::OuterBevel: e.width = 6; e.softness = 3; e.opacity = 75; e.color = QColor(0x99, 0x99, 0x99); e.angle = 135; break;
    case EffectType::InsetEmboss: case EffectType::RaisedEmboss: e.width = 5; e.softness = 3; e.opacity = 75; e.angle = 135; break;
    case EffectType::GaussianBlur: e.softness = 2; break;
    case EffectType::UnsharpMask: e.amount = 50; e.softness = 1; e.threshold = 0; break;
    case EffectType::Noise: e.amount = 15; break;
    case EffectType::ColorFill: e.color = QColor(0xFF, 0x66, 0x00); e.opacity = 50; break;
    case EffectType::InnerGlow: e.width = 4; e.softness = 4; e.opacity = 75; e.color = QColor(0xFF, 0xFF, 0x99); break;
    case EffectType::MotionBlur: e.angle = 0; e.distance = 10; break;
    case EffectType::RadialBlur: e.amount = 10; e.quality = 20; break;
    case EffectType::ZoomBlur: e.amount = 20; e.quality = 20; break;
    case EffectType::Threshold: e.threshold = 128; break;
    case EffectType::Posterize: e.posterize = 4; break;
    case EffectType::HighPass: e.softness = 10; e.keepColor = true; break;
    case EffectType::Offset: e.offsetX = 0; e.offsetY = 0; e.undefinedAreas = LiveEffect::WrapAround; break;
    case EffectType::PhotoFilter: e.color = QColor(0xEC, 0x8A, 0x00); e.amount = 25; break; // "Warming (85)"
    case EffectType::FeatherEdges: e.width = 10; e.featherCentered = false; e.amount = 100; break;
    // v0.3.0
    case EffectType::GradientMap: e.stops = {{0, QColor(0, 0, 0)}, {1, QColor(255, 255, 255)}}; e.opacity = 100; e.flag = false; break;
    case EffectType::Dither: e.mode = 0; e.mode2 = 2; e.posterize = 16; e.amount = 100; e.flag = true; break; // Floyd-Steinberg to 1-bit, serpentine
    case EffectType::Outline: e.mode = 0; e.mode2 = 1; e.width = 1; e.color = Qt::black; e.threshold = 128; e.opacity = 100; break;
    case EffectType::PaletteSwap: e.flag = false; break; // flag: nearest colour (else exact matches only)
    case EffectType::NormalMap: e.mode = 0; e.mode2 = 0; e.amount = 2; e.flag = false; e.flag2 = true; break; // Sobel, luminance, OpenGL (+Y up), wrap
    case EffectType::AmbientOcclusion: e.mode2 = 0; e.amount = 100; e.width = 8; e.flag2 = true; break;
    case EffectType::LensCorrection: break; // p[0] distortion, p[1] CA red/cyan, p[2] CA blue/yellow, p[3] vignette, p[4] midpoint, p[5] vertical, p[6] horizontal perspective, p[7] angle
    case EffectType::SmartSharpen: e.amount = 100; e.softness = 1; e.mode = 0; e.p[0] = 10; e.angle = 0; break;
    case EffectType::SurfaceBlur: e.softness = 5; e.threshold = 15; break;
    case EffectType::BilateralDenoise: e.softness = 3; e.amount = 25; e.quality = 1; break;
    case EffectType::MakeSeamless: e.mode = 0; e.width = 32; break;
    // v0.3.1: color From, color2 To, amount tolerance, p[0] fuzziness falloff, mode range (Solid/Color/Shift), opacity strength, flag hard
    case EffectType::ReplaceColor: e.color = QColor(255, 0, 0); e.color2 = QColor(0, 0, 255); e.amount = 20; e.p[0] = 50; e.mode = 0; e.opacity = 100; e.flag = false; break;
    default: break;
    }
    return e;
}

static const struct { EffectType t; const char *key; const char *label; const char *cat; } kFx[] = {
    {EffectType::AutoContrast, "autoContrast", "Auto Contrast", "Adjust Color"},
    {EffectType::AutoLevels, "autoLevels", "Auto Levels", "Adjust Color"},
    {EffectType::BrightnessContrast, "brightnessContrast", "Brightness/Contrast", "Adjust Color"},
    {EffectType::ColorBalance, "colorBalance", "Color Balance", "Adjust Color"},
    {EffectType::ColorFill, "colorFill", "Color Fill", "Adjust Color"},
    {EffectType::Curves, "curves", "Curves", "Adjust Color"},
    {EffectType::Desaturate, "desaturate", "Desaturate", "Adjust Color"},
    {EffectType::GradientMap, "gradientMap", "Gradient Map", "Adjust Color"},
    {EffectType::HueSaturation, "hueSaturation", "Hue/Saturation", "Adjust Color"},
    {EffectType::Invert, "invert", "Invert", "Adjust Color"},
    {EffectType::Levels, "levels", "Levels", "Adjust Color"},
    {EffectType::PhotoFilter, "photoFilter", "Photo Filter", "Adjust Color"},
    {EffectType::ReplaceColor, "replaceColor", "Replace Color", "Adjust Color"},
    {EffectType::Posterize, "posterize", "Posterize", "Adjust Color"},
    {EffectType::Threshold, "threshold", "Threshold", "Adjust Color"},
    {EffectType::InnerBevel, "innerBevel", "Inner Bevel", "Bevel and Emboss"},
    {EffectType::OuterBevel, "outerBevel", "Outer Bevel", "Bevel and Emboss"},
    {EffectType::InsetEmboss, "insetEmboss", "Inset Emboss", "Bevel and Emboss"},
    {EffectType::RaisedEmboss, "raisedEmboss", "Raised Emboss", "Bevel and Emboss"},
    {EffectType::Blur, "blur", "Blur", "Blur"},
    {EffectType::BlurMore, "blurMore", "Blur More", "Blur"},
    {EffectType::GaussianBlur, "gaussianBlur", "Gaussian Blur", "Blur"},
    {EffectType::MotionBlur, "motionBlur", "Motion Blur", "Blur"},
    {EffectType::RadialBlur, "radialBlur", "Radial Blur", "Blur"},
    {EffectType::SurfaceBlur, "surfaceBlur", "Surface Blur", "Blur"},
    {EffectType::ZoomBlur, "zoomBlur", "Zoom Blur", "Blur"},
    {EffectType::LensCorrection, "lensCorrection", "Lens Correction", "Distort"},
    {EffectType::ConvertToAlpha, "convertToAlpha", "Convert to Alpha", "Other"},
    {EffectType::FindEdges, "findEdges", "Find Edges", "Other"},
    {EffectType::HighPass, "highPass", "High Pass", "Other"},
    {EffectType::Offset, "offset", "Offset", "Other"},
    {EffectType::FeatherEdges, "featherEdges", "Feather Edges", "Other"},
    {EffectType::Noise, "noise", "Add Noise", "Noise"},
    {EffectType::BilateralDenoise, "bilateralDenoise", "Reduce Noise (Bilateral)", "Noise"},
    {EffectType::Dither, "dither", "Dither", "Pixel Art"},
    {EffectType::Outline, "outline", "Outline", "Pixel Art"},
    {EffectType::PaletteSwap, "paletteSwap", "Palette Swap", "Pixel Art"},
    {EffectType::DropShadow, "dropShadow", "Drop Shadow", "Shadow and Glow"},
    {EffectType::OuterGlow, "outerGlow", "Glow", "Shadow and Glow"},
    {EffectType::InnerGlow, "innerGlow", "Inner Glow", "Shadow and Glow"},
    {EffectType::InnerShadow, "innerShadow", "Inner Shadow", "Shadow and Glow"},
    {EffectType::Sharpen, "sharpen", "Sharpen", "Sharpen"},
    {EffectType::SharpenMore, "sharpenMore", "Sharpen More", "Sharpen"},
    {EffectType::SmartSharpen, "smartSharpen", "Smart Sharpen", "Sharpen"},
    {EffectType::UnsharpMask, "unsharpMask", "Unsharp Mask", "Sharpen"},
    {EffectType::AmbientOcclusion, "ambientOcclusion", "Ambient Occlusion from Height", "Texture"},
    {EffectType::MakeSeamless, "makeSeamless", "Make Seamless", "Texture"},
    {EffectType::NormalMap, "normalMap", "Normal Map from Height", "Texture"},
};
QString effectCategory(EffectType t) { for (auto &f : kFx) if (f.t == t) return QString::fromLatin1(f.cat); return {}; }
bool effectIsAdjustment(EffectType t) {
    switch (t) {
    case EffectType::DropShadow: case EffectType::InnerShadow: case EffectType::OuterGlow: case EffectType::InnerBevel:
    case EffectType::OuterBevel: case EffectType::InsetEmboss: case EffectType::RaisedEmboss: case EffectType::InnerGlow:
        return false;
    default: return true;
    }
}
QStringList channelNames() { return {QStringLiteral("RGB"), QStringLiteral("Red"), QStringLiteral("Green"), QStringLiteral("Blue"), QStringLiteral("Alpha")}; }
QStringList bevelPresetNames() { return {QStringLiteral("Raised"), QStringLiteral("Highlighted"), QStringLiteral("Inset"), QStringLiteral("Inverted")}; }
QString effectName(EffectType t) { for (auto &f : kFx) if (f.t == t) return QString::fromLatin1(f.label); return {}; }
QString effectKey(EffectType t) { for (auto &f : kFx) if (f.t == t) return QString::fromLatin1(f.key); return {}; }
EffectType effectFromKey(const QString &k, bool *ok) {
    for (auto &f : kFx) if (k == QLatin1String(f.key)) { if (ok) *ok = true; return f.t; }
    if (ok) *ok = false;
    return EffectType::DropShadow;
}
QList<EffectType> allEffectTypes() { QList<EffectType> l; for (auto &f : kFx) l << f.t; return l; }

static const struct { TextAA a; const char *key; const char *label; } kTextAA[] = {
    {TextAA::None, "none", "No Anti-Alias"}, {TextAA::Crisp, "crisp", "Crisp Anti-Alias"},
    {TextAA::Strong, "strong", "Strong Anti-Alias"}, {TextAA::Smooth, "smooth", "Smooth Anti-Alias"},
    {TextAA::System, "system", "System Anti-Alias"},
};
QStringList textAANames() { QStringList l; for (auto &x : kTextAA) l << QString::fromLatin1(x.label); return l; }
QString textAAKey(TextAA a) { for (auto &x : kTextAA) if (x.a == a) return QString::fromLatin1(x.key); return QStringLiteral("smooth"); }
TextAA textAAFromKey(const QString &k, bool *ok) {
    for (auto &x : kTextAA) if (k == QLatin1String(x.key)) { if (ok) *ok = true; return x.a; }
    if (ok) *ok = false;
    return TextAA::Smooth;
}

static const struct { ObjType t; const char *key; const char *label; } kTypes[] = {
    {ObjType::Bitmap, "bitmap", "Bitmap"}, {ObjType::Rect, "rect", "Rectangle"},
    {ObjType::Ellipse, "ellipse", "Ellipse"}, {ObjType::Line, "line", "Line"},
    {ObjType::Path, "path", "Path"}, {ObjType::Text, "text", "Text"},
    {ObjType::Slice, "slice", "Slice"}, {ObjType::Hotspot, "hotspot", "Hotspot"},
    {ObjType::Instance, "instance", "Symbol Instance"},
    {ObjType::AutoShape, "autoshape", "Auto Shape"},
    {ObjType::Group, "group", "Group"},
};

QString symbolKindKey(SymbolKind k) {
    switch (k) {
    case SymbolKind::Animation: return QStringLiteral("animation");
    case SymbolKind::Button: return QStringLiteral("button");
    default: return QStringLiteral("graphic");
    }
}
QString symbolKindLabel(SymbolKind k) {
    switch (k) {
    case SymbolKind::Animation: return QStringLiteral("Animation");
    case SymbolKind::Button: return QStringLiteral("Button");
    default: return QStringLiteral("Graphic");
    }
}
SymbolKind symbolKindFromKey(const QString &k) {
    if (k == QLatin1String("animation")) return SymbolKind::Animation;
    if (k == QLatin1String("button")) return SymbolKind::Button;
    return SymbolKind::Graphic;
}
QStringList buttonStateNames() {
    return {QStringLiteral("Up"), QStringLiteral("Over"), QStringLiteral("Down"), QStringLiteral("Over While Down")};
}

QRectF Symbol::bounds() const {
    QRectF b;
    for (const auto &f : frames) for (const VObject &o : f) if (!o.isWeb()) b = b.isNull() ? o.bounds() : b.united(o.bounds());
    if (b.width() < 1e-6) b.setWidth(1);
    if (b.height() < 1e-6) b.setHeight(1);
    return b;
}

QRectF Symbol::visualBounds() const {
    // A symbol may contain an instance of itself; don't recurse forever (render already caps depth).
    static thread_local QSet<quint64> stack;
    if (stack.contains(id)) return bounds();
    stack.insert(id);
    QRectF b;
    for (const auto &f : frames) for (const VObject &o : f) if (!o.isWeb()) b = b.isNull() ? o.visualBounds() : b.united(o.visualBounds());
    stack.remove(id);
    return b.isNull() ? bounds() : b;
}

QRectF Symbol::artworkBounds() const {
    static thread_local QSet<quint64> stack;
    if (stack.contains(id)) return bounds();
    stack.insert(id);
    QRectF b;
    for (const auto &f : frames) for (const VObject &o : f) if (!o.isWeb()) b = b.isNull() ? o.artworkBounds() : b.united(o.artworkBounds());
    stack.remove(id);
    return b.isNull() ? bounds() : b;
}

namespace {
thread_local const QVector<Symbol> *t_symbols = nullptr;
}
SymbolScope::SymbolScope(const QVector<Symbol> *syms) : m_prev(t_symbols) { if (syms) t_symbols = syms; }
SymbolScope::~SymbolScope() { t_symbols = m_prev; }
const Symbol *findSymbol(quint64 id) {
    if (!t_symbols) return nullptr;
    for (const Symbol &s : *t_symbols) if (s.id == id) return &s;
    return nullptr;
}
QTransform instanceTransform(const VObject &inst, const Symbol &sym) {
    const QRectF b = sym.bounds();
    const QRectF r = inst.rect.normalized();
    QTransform t;
    t.translate(r.left(), r.top());
    t.scale(r.width() / b.width(), r.height() / b.height());
    t.translate(-b.left(), -b.top());
    return t * inst.xform;   // symbol -> placement box -> the instance's own rotation / skew / distortion
}
QString objTypeKey(ObjType t) { for (auto &x : kTypes) if (x.t == t) return QString::fromLatin1(x.key); return {}; }
QString objTypeLabel(ObjType t) { for (auto &x : kTypes) if (x.t == t) return QString::fromLatin1(x.label); return {}; }
ObjType objTypeFromKey(const QString &k, bool *ok) {
    for (auto &x : kTypes) if (k == QLatin1String(x.key)) { if (ok) *ok = true; return x.t; }
    if (ok) *ok = false;
    return ObjType::Rect;
}

QRectF VObject::bounds() const {
    if (type == ObjType::Group) {
        QRectF b;
        for (const VObject &c : children) if (!c.isWeb()) { const QRectF cb = c.bounds(); b = b.isNull() ? cb : b.united(cb); }
        if (maskActive()) b = b.intersected(maskObj[0].bounds());
        return b;
    }
    if (hasXform()) return xform.mapRect(untransformed().bounds());
    switch (type) {
    case ObjType::Line: return QRectF(p1, p2).normalized();
    case ObjType::Path: return hasCurves() ? shapePath().boundingRect() : points.boundingRect();
    case ObjType::Text: if (textOnPath && points.size() > 1) { QRectF r = textPath().boundingRect(); return r.isEmpty() ? guidePath().boundingRect() : r; } return rect.normalized();
    case ObjType::Slice:
    case ObjType::Hotspot: if (webShape == WebShape::Polygon) return points.boundingRect(); return rect.normalized();
    case ObjType::AutoShape: {
        // the rect, plus anything drawn outside it (speech bubble tail, line end points)
        const QRectF g = autoshape::outline(*this).boundingRect();
        return g.isNull() ? rect.normalized() : rect.normalized().united(g);
    }
    default: return rect.normalized();
    }
}

// Room the effects need around the object. Effects run as a pipeline, each one growing what the
// previous ones produced, so the expansions add up (a blur after a glow spreads the glow further).
namespace {
double effectPad(const VObject &o, bool reach) {
    double m = 0;
    double radius = 0;
    if (reach) {
        const QRectF b = o.hasXform() ? o.xform.mapRect(o.untransformed().bounds()) : o.bounds();
        radius = std::hypot(b.width(), b.height()) / 2.0;
    }
    for (const auto &e : o.effects) {
        if (!e.enabled) continue;
        switch (e.type) {
        case EffectType::DropShadow: m += e.distance + e.softness * 3 + 2; break;
        case EffectType::OuterGlow: m += e.width + e.softness * 3 + 2; break;
        case EffectType::GaussianBlur: m += e.softness * 3 + 2; break;
        case EffectType::OuterBevel: m += e.width + e.softness + 3; break;
        case EffectType::Blur: m += 4.0; break;
        case EffectType::BlurMore: m += 9.0; break;
        case EffectType::MotionBlur: m += e.distance + 2; break;
        case EffectType::RadialBlur:
            if (reach) {
                const double rad = qDegreesToRadians(std::abs(e.amount) * 0.5);
                m += radius * 2.0 * std::abs(std::sin(rad / 2.0)) + 2;
            } else {
                m += 2;
            }
            break;
        case EffectType::ZoomBlur:
            if (reach) {
                const double scMin = 1.0 - e.amount / 100.0;
                if (scMin > 0.05) m += radius * (1.0 / scMin - 1.0) + 2;
                else m += radius * 20.0 + 2;
            } else {
                m += 2;
            }
            break;
        case EffectType::FeatherEdges: if (e.featherCentered) m += e.width / 2.0 + 2; break;
        case EffectType::Outline: if (e.mode == 0) m += qMax(1.0, e.width) + 1; break;
        case EffectType::SmartSharpen: if (e.mode == 2) m += 1; break;
        default: break;
        }
    }
    return m;
}
}

double VObject::effectMargin() const { return effectPad(*this, false); }

double VObject::effectReach() const {
    // Same as effectMargin for tightly-bounded effects; Radial / Zoom use the object's
    // half-diagonal so the renderObject early-out does not skip a clip the effect can touch.
    // This does not change visualBounds / the render buffer (output stays pixel-identical).
    return effectPad(*this, true);
}

QRectF VObject::visualBounds() const {
    if (type == ObjType::Group) {
        QRectF b;
        for (const VObject &c : children) if (!c.isWeb()) { const QRectF cb = c.visualBounds(); b = b.isNull() ? cb : b.united(cb); }
        if (maskActive()) b = b.intersected(maskObj[0].visualBounds());
        if (b.isEmpty()) return b;
        const double m = 1 + effectMargin();
        return b.adjusted(-m, -m, m, m);
    }
    if (hasXform()) {
        // stroke / feather / symbol extent travel with the transform; effects work on the canvas
        VObject c = untransformed();
        c.effects.clear();
        const double m = effectMargin() + 1;
        return xform.mapRect(c.visualBounds()).adjusted(-m, -m, m, m);
    }
    QRectF b = bounds();
    if (isWeb()) return b.adjusted(-1, -1, 1, 1);
    if (type == ObjType::Instance) {
        if (const Symbol *sym = findSymbol(symbolId)) b = instanceTransform(*this, *sym).mapRect(sym->visualBounds());
        const double m = 1 + effectMargin();
        return b.adjusted(-m, -m, m, m);
    }
    const double sw = strokeWidth * maxPointSize();   // Path Scrubber: up to 2x the width
    double s = (strokeOn && type != ObjType::Bitmap) ? (strokeAlign == 2 ? sw : strokeAlign == 1 ? 0 : sw / 2.0) + 1 + fills::strokeExtent(*this) : 1;
    if (fillOn && feather > 0) s = qMax(s, feather * 2 + 1);
    if (type == ObjType::Text) s += 2; // glyph overhang and Strong AA embolden
    double m = s + effectMargin();
    return b.adjusted(-m, -m, m, m);
}

QRectF VObject::artworkBounds() const {
    // Like visualBounds, but without the blanket antialias pad. A plain bitmap
    // (no stroke, effects, transform or feather) is exactly image.size().
    if (type == ObjType::Group) {
        QRectF b;
        for (const VObject &c : children) if (!c.isWeb()) {
            const QRectF cb = c.artworkBounds();
            b = b.isNull() ? cb : b.united(cb);
        }
        if (maskActive()) b = b.intersected(maskObj[0].artworkBounds());
        if (b.isEmpty()) return b;
        const double m = effectMargin();
        return m > 0 ? b.adjusted(-m, -m, m, m) : b;
    }
    if (type == ObjType::Bitmap && !hasXform() && effects.isEmpty() && !strokeOn && feather <= 0) {
        const QSize sz = image.isNull() ? bounds().size().toSize() : image.size();
        return QRectF(bounds().topLeft(), QSizeF(sz));
    }
    if (hasXform()) {
        VObject c = untransformed();
        c.effects.clear();
        const double m = effectMargin();
        const QRectF b = xform.mapRect(c.artworkBounds());
        return m > 0 ? b.adjusted(-m, -m, m, m) : b;
    }
    QRectF b = bounds();
    if (isWeb()) return b;
    if (type == ObjType::Instance) {
        if (const Symbol *sym = findSymbol(symbolId)) b = instanceTransform(*this, *sym).mapRect(sym->artworkBounds());
        const double m = effectMargin();
        return m > 0 ? b.adjusted(-m, -m, m, m) : b;
    }
    const double sw = strokeWidth * maxPointSize();
    double s = (strokeOn && type != ObjType::Bitmap) ? (strokeAlign == 2 ? sw : strokeAlign == 1 ? 0 : sw / 2.0) + fills::strokeExtent(*this) : 0;
    if (fillOn && feather > 0) s = qMax(s, feather * 2);
    if (type == ObjType::Text) s += 2;
    const double m = s + effectMargin();
    return m > 0 ? b.adjusted(-m, -m, m, m) : b;
}

QSize artworkPixelSize(const QRectF &area) {
    if (area.isEmpty()) return QSize();
    // ceil of the box size only — not toAlignedRect(), which does
    // ceil(right)-floor(left) and grows a fractional origin by 1 px.
    const int w = qMax(1, int(std::ceil(area.width() - 1e-9)));
    const int h = qMax(1, int(std::ceil(area.height() - 1e-9)));
    return QSize(w, h);
}

QSize clipboardCanvasSize(const QVector<VObject> &objs) {
    QRectF area;
    for (const VObject &o : objs) {
        const QRectF b = o.artworkBounds();
        area = area.isNull() ? b : area.united(b);
    }
    return artworkPixelSize(area);
}

void VObject::moveBy(const QPointF &d) {
    if (type == ObjType::Group) {
        for (VObject &c : children) c.moveBy(d);
        if (maskLinked) for (VObject &m : maskObj) m.moveBy(d);
        return;
    }
    if (hasXform()) { xform *= QTransform::fromTranslate(d.x(), d.y()); return; }
    rect.translate(d);
    p1 += d; p2 += d;
    points.translate(d);
    hIn.translate(d);
    hOut.translate(d);
}

bool VObject::hasCurves() const {
    if (hIn.size() != points.size() || hOut.size() != points.size()) return false;
    for (int i = 0; i < points.size(); ++i)
        if (hIn[i] != points[i] || hOut[i] != points[i]) return true;
    return false;
}

void VObject::ensureHandles() {
    if (hIn.size() != points.size()) hIn = points;
    if (hOut.size() != points.size()) hOut = points;
}

bool VObject::hasPointPressure() const {
    if (type != ObjType::Path || textOnPath) return false;
    const int n = points.size();
    if (ptSize.size() == n) for (double v : ptSize) if (v != 1.0) return true;
    if (ptOpacity.size() == n) for (double v : ptOpacity) if (v != 1.0) return true;
    return false;
}

double VObject::pointSize(int i) const {
    if (ptSize.size() != points.size() || i < 0 || i >= ptSize.size()) return 1.0;
    const double v = ptSize[i];
    return std::isfinite(v) ? qBound(0.0, v, kMaxPointSize) : 1.0;
}

double VObject::pointOpacity(int i) const {
    if (ptOpacity.size() != points.size() || i < 0 || i >= ptOpacity.size()) return 1.0;
    const double v = ptOpacity[i];
    return std::isfinite(v) ? qBound(0.0, v, 1.0) : 1.0;
}

double VObject::maxPointSize() const {
    double m = 1.0;
    if (type == ObjType::Path && ptSize.size() == points.size())
        for (int i = 0; i < ptSize.size(); ++i) m = qMax(m, pointSize(i));
    return m;
}

bool VObject::hasPointSense() const {
    if (type != ObjType::Path || textOnPath) return false;
    const int n = points.size();
    if (ptPressure.size() == n) for (double v : ptPressure) if (std::abs(v - 1.0) > 1e-6) return true;
    if (ptSpeed.size() == n) for (double v : ptSpeed) if (std::abs(v - 1.0) > 1e-6) return true;
    return false;
}

double VObject::pointPressure(int i) const {
    if (ptPressure.size() != points.size() || i < 0 || i >= ptPressure.size()) return 1.0;
    const double v = ptPressure[i];
    return std::isfinite(v) ? qBound(0.0, v, 1.0) : 1.0;
}

double VObject::pointSpeed(int i) const {
    if (ptSpeed.size() != points.size() || i < 0 || i >= ptSpeed.size()) return 1.0;
    const double v = ptSpeed[i];
    return std::isfinite(v) ? qBound(0.0, v, kMaxPointSpeed) : 1.0;
}

void VObject::ensurePointPressure() {
    const int n = points.size();
    if (ptSize.size() != n) ptSize = QVector<double>(n, 1.0);
    if (ptOpacity.size() != n) ptOpacity = QVector<double>(n, 1.0);
}

void VObject::dropTrivialHandles() {
    if (!hasCurves()) { hIn.clear(); hOut.clear(); }
}

void VObject::contourRange(int c, int *start, int *end) const {
    const int n = points.size();
    int a = c == 0 ? 0 : (c - 1 < contours.size() ? contours[c - 1] : n);
    int b = c < contours.size() ? contours[c] : n;
    *start = qBound(0, a, n);
    *end = qBound(*start, b, n);
}

int VObject::contourOfPoint(int i) const {
    int c = 0;
    while (c < contours.size() && i >= contours[c]) ++c;
    return c;
}

void VObject::setBounds(const QRectF &nb) {
    QRectF old = bounds();
    if (type == ObjType::Group) {
        // every member keeps its place inside the group's box
        auto mapRect = [&](const QRectF &r) {
            auto m = [&](QPointF p) {
                const double fx = old.width() > 1e-9 ? (p.x() - old.left()) / old.width() : 0.0;
                const double fy = old.height() > 1e-9 ? (p.y() - old.top()) / old.height() : 0.0;
                return QPointF(nb.left() + fx * nb.width(), nb.top() + fy * nb.height());
            };
            return QRectF(m(r.topLeft()), m(r.bottomRight()));
        };
        for (VObject &c : children) c.setBounds(mapRect(c.bounds()));
        if (maskLinked) for (VObject &mk : maskObj) mk.setBounds(mapRect(mk.bounds()));
        return;
    }
    if (hasXform()) {
        // scale the transformed object on the canvas: compose instead of touching the geometry
        const double sx = old.width() > 1e-9 ? nb.width() / old.width() : 1, sy = old.height() > 1e-9 ? nb.height() / old.height() : 1;
        xform *= QTransform::fromTranslate(-old.left(), -old.top()) * QTransform::fromScale(sx, sy) * QTransform::fromTranslate(nb.left(), nb.top());
        return;
    }
    auto map = [&](QPointF p) {
        double fx = old.width() > 1e-9 ? (p.x() - old.left()) / old.width() : 0.0;
        double fy = old.height() > 1e-9 ? (p.y() - old.top()) / old.height() : 0.0;
        return QPointF(nb.left() + fx * nb.width(), nb.top() + fy * nb.height());
    };
    switch (type) {
    case ObjType::Line: p1 = map(p1); p2 = map(p2); break;
    case ObjType::Text:
        if (!textOnPath) { rect = nb; break; }
        [[fallthrough]];
    case ObjType::Path:
        for (auto &p : points) p = map(p);
        for (auto &p : hIn) p = map(p);
        for (auto &p : hOut) p = map(p);
        break;
    case ObjType::Slice:
    case ObjType::Hotspot:
        if (webShape == WebShape::Polygon) {
            for (auto &p : points) p = map(p);
            rect = points.boundingRect();
            break;
        }
        rect = nb; break;
    case ObjType::AutoShape: {
        // bounds may extend past rect (speech bubble tail): map rect's corners
        const QRectF r = rect.normalized();
        rect = QRectF(map(r.topLeft()), map(r.bottomRight())).normalized();
        break;
    }
    default: rect = nb; break;
    }
}

QPainterPath VObject::shapePath() const {
    if (type == ObjType::Group) {
        QPainterPath all;
        for (const VObject &c : children) if (!c.isWeb()) all.addPath(c.shapePath());
        return all;
    }
    if (hasXform()) return xform.map(untransformed().shapePath());
    QPainterPath path;
    switch (type) {
    case ObjType::Rect: {
        QRectF r = rect.normalized();
        if (roundness > 0) {
            double rad = roundness / 100.0 * qMin(r.width(), r.height()) / 2.0;
            path.addRoundedRect(r, rad, rad);
        } else path.addRect(r);
        break;
    }
    case ObjType::Ellipse: path.addEllipse(rect.normalized()); break;
    case ObjType::AutoShape: path = autoshape::outline(*this); break;
    case ObjType::Line: path.moveTo(p1); path.lineTo(p2); break;
    case ObjType::Path:
        path = paths::nodesToPath(*this);
        break;
    case ObjType::Slice:
    case ObjType::Hotspot:
        if (webShape == WebShape::Circle) path.addEllipse(rect.normalized());
        else if (webShape == WebShape::Polygon) { if (points.size() > 1) { path.addPolygon(points); path.closeSubpath(); } }
        else path.addRect(rect.normalized());
        break;
    default: path.addRect(rect.normalized()); break;
    }
    return path;
}

bool VObject::hitTest(const QPointF &p, double tol) const {
    if (!visible) return false;
    if (type == ObjType::Group) {
        if (maskActive() && !maskObj[0].bounds().adjusted(-tol / 2, -tol / 2, tol / 2, tol / 2).contains(p)) return false;
        for (int i = children.size() - 1; i >= 0; --i) if (children[i].hitTest(p, tol)) return true;
        return false;
    }
    if (hasXform()) {
        bool inv = false;
        const QTransform i = xform.inverted(&inv);
        if (!inv) return false;
        const double sc = qSqrt(qMax(1e-9, qAbs(xform.determinant())));
        return untransformed().hitTest(i.map(p), tol / sc);
    }
    switch (type) {
    case ObjType::Text:
        if (textOnPath && points.size() > 1) return bounds().adjusted(-tol / 2, -tol / 2, tol / 2, tol / 2).contains(p);
        [[fallthrough]];
    case ObjType::Bitmap:
    case ObjType::Instance:
        return rect.normalized().adjusted(-tol / 2, -tol / 2, tol / 2, tol / 2).contains(p);
    case ObjType::Slice:
    case ObjType::Hotspot: {
        QPainterPath path = shapePath();
        if (path.contains(p)) return true;
        QPainterPathStroker st;
        st.setWidth(1 + tol);
        return st.createStroke(path).contains(p);
    }
    case ObjType::AutoShape: {
        const autoshape::Geometry g = autoshape::geometry(*this);
        if (fillOn && g.fill.contains(p)) return true;
        if (g.heads.contains(p)) return true;
        QPainterPath all = g.fill;
        all.addPath(g.open);
        all.addPath(g.heads);
        QPainterPathStroker st;
        st.setWidth(qMax(strokeOn ? strokeWidth : 1.0, 1.0) + tol);
        return st.createStroke(all).contains(p);
    }
    default: {
        QPainterPath path = shapePath();
        bool fillable = fillOn && type != ObjType::Line;
        if (fillable && path.contains(p)) return true;
        QPainterPathStroker st;
        st.setWidth(qMax(strokeOn ? strokeWidth : 1.0, 1.0) + tol);
        return st.createStroke(path).contains(p);
    }
    }
}

QFont VObject::font() const {
    QFont f(fontFamily);
    f.setPixelSize(qMax(1, qRound(fontSize)));
    f.setBold(bold);
    f.setItalic(italic);
    f.setUnderline(underline);
    if (!qFuzzyIsNull(kerning)) f.setLetterSpacing(QFont::AbsoluteSpacing, kerning / 100.0 * fontSize);
    if (!qFuzzyCompare(hScale, 100.0)) f.setStretch(qBound(1, qRound(hScale), 4000));
    // On the raster engine the TextAntialiasing render hint alone does not reliably
    // disable glyph anti-aliasing; the font's style strategy decides what FreeType renders.
    int strat = QFont::NoSubpixelAntialias; // grey AA only: we render into transparent images
    switch (textAA) {
    case TextAA::None: strat = QFont::NoAntialias; f.setHintingPreference(QFont::PreferFullHinting); break;
    case TextAA::Crisp: strat |= QFont::PreferAntialias; f.setHintingPreference(QFont::PreferFullHinting); break;
    case TextAA::Strong: strat |= QFont::PreferAntialias; f.setHintingPreference(QFont::PreferVerticalHinting); break;
    case TextAA::Smooth: strat |= QFont::PreferAntialias; f.setHintingPreference(QFont::PreferNoHinting); break;
    case TextAA::System: strat = QFont::PreferDefault; f.setHintingPreference(QFont::PreferDefaultHinting); break;
    }
    f.setStyleStrategy(QFont::StyleStrategy(strat));
    return f;
}

static QString visualRtl(const QString &s)
{
    QString o;
    o.reserve(s.size());
    for (int i = s.size() - 1; i >= 0; --i) o.append(s.at(i));
    return o;
}

QVector<VObject::TextLine> VObject::textLines() const {
    QVector<TextLine> out;
    const QFont f = font();
    QFontMetricsF fm(f);
    const QRectF r = rect.normalized();
    const bool vertical = textOrientation == 1;
    const bool rtl = textDirection == 1;
    const double spacing = lineAdvance();
    const QStringList paras = text.split('\n');

    if (!vertical) {
        double y = r.top() + fm.ascent() - baselineShift;
        for (int pi = 0; pi < paras.size(); ++pi) {
            y += paraSpaceBefore;
            const QString &para = paras[pi];
            QStringList words = para.split(' ');
            QVector<QString> lines;
            QString line;
            const auto wrapW = [&](bool first) {
                return qMax(1.0, r.width() - (first ? paraIndent : 0));
            };
            for (int i = 0; i < words.size(); ++i) {
                const bool first = lines.isEmpty();
                QString cand = line.isEmpty() && i == 0 ? words[i] : line + ' ' + words[i];
                if (i > 0 && fm.horizontalAdvance(cand) > wrapW(first) + 0.5 && !line.isEmpty()) {
                    lines.append(line);
                    line = words[i];
                } else line = cand;
            }
            lines.append(line);
            for (int li = 0; li < lines.size(); ++li) {
                const QString raw = lines[li];
                const QString vis = rtl ? visualRtl(raw) : raw;
                const bool first = li == 0;
                const bool last = li + 1 == lines.size();
                const double indent = first ? paraIndent : 0;
                const double left = r.left() + indent;
                const double avail = qMax(1.0, r.right() - left);
                const double w = fm.horizontalAdvance(vis);
                if (textAlign == 3 && !last && raw.contains(QLatin1Char(' '))) {
                    const QStringList ws = raw.split(' ');
                    const QStringList vws = rtl ? [&]{ QStringList v; for (int i = ws.size()-1; i >= 0; --i) v << visualRtl(ws[i]); return v; }() : ws;
                    double natural = 0;
                    for (int i = 0; i < vws.size(); ++i) natural += fm.horizontalAdvance(vws[i]);
                    const double space = vws.size() > 1 ? (avail - natural) / (vws.size() - 1) : 0;
                    double x = left;
                    if (rtl) {
                        x = r.right();
                        for (int i = 0; i < vws.size(); ++i) {
                            x -= fm.horizontalAdvance(vws[i]);
                            out.append({QPointF(x, y), vws[i]});
                            x -= space;
                        }
                    } else {
                        for (int i = 0; i < vws.size(); ++i) {
                            out.append({QPointF(x, y), vws[i]});
                            x += fm.horizontalAdvance(vws[i]) + space;
                        }
                    }
                } else {
                    double x = left;
                    if (textAlign == 1) x = left + (avail - w) / 2;
                    else if (textAlign == 2) x = r.right() - w;
                    out.append({QPointF(x, y), vis});
                }
                y += spacing;
            }
            y += paraSpaceAfter;
        }
        return out;
    }

    // Vertical: upright glyphs stacked down a column, then the next column.
    const double colW = qMax(fm.horizontalAdvance(QLatin1Char('M')), fm.height() * 0.6) * qMax(0.15, leading / 100.0);
    double x = rtl ? (r.right() - colW * 0.5) : (r.left() + colW * 0.5);
    const double wrapH = qMax(1.0, r.height());
    for (int pi = 0; pi < paras.size(); ++pi) {
        const QString &para = paras[pi];
        QString chars;
        for (int i = 0; i < para.size(); ++i) {
            QString ch = para.mid(i, 1);
            if (para[i].isHighSurrogate() && i + 1 < para.size()) { ch = para.mid(i, 2); ++i; }
            chars += ch;
        }
        if (rtl) chars = visualRtl(chars);
        double y = r.top() + fm.ascent() - baselineShift + paraSpaceBefore + paraIndent;
        auto emitCh = [&](const QString &ch) {
            if (y - r.top() + fm.descent() > wrapH + 0.5 && y > r.top() + fm.ascent() + paraSpaceBefore + paraIndent + 0.5) {
                x += rtl ? -colW : colW;
                y = r.top() + fm.ascent() - baselineShift + paraSpaceBefore;
            }
            out.append({QPointF(x - fm.horizontalAdvance(ch) / 2, y), ch});
            y += spacing;
        };
        if (chars.isEmpty()) {
            y += spacing;
        } else {
            // walk by code units already grouped in chars... emit one-by-one
            for (int i = 0; i < chars.size(); ++i) {
                QString ch = chars.mid(i, 1);
                if (chars[i].isHighSurrogate() && i + 1 < chars.size()) { ch = chars.mid(i, 2); ++i; }
                emitCh(ch);
            }
        }
        y += paraSpaceAfter;
        Q_UNUSED(y);
    }
    return out;
}

QPainterPath VObject::guidePath() const { return paths::nodesToPath(*this); }

QPainterPath VObject::textPath() const {
    if (hasXform()) return xform.map(untransformed().textPath());
    QPainterPath tp;
    const QFont f = font();
    if (textOnPath && points.size() > 1) {
        // lay glyphs along the guide, rotated to its tangent (Text > Attach to Path)
        const QPainterPath g = guidePath();
        const double len = g.length();
        QFontMetricsF fm(f);
        QString t = text;
        t.replace('\n', ' ');
        double total = fm.horizontalAdvance(t);
        double start = textPathOffset;
        if (textAlign == 1) start += (len - total) / 2;
        else if (textAlign == 2) start += len - total;
        if (textDirection == 1) t = visualRtl(t);
        double x = start;
        for (int i = 0; i < t.size(); ++i) {
            QString ch = t.mid(i, 1);
            if (t[i].isHighSurrogate() && i + 1 < t.size()) { ch = t.mid(i, 2); ++i; }
            const double adv = fm.horizontalAdvance(ch);
            const double mid = x + adv / 2;
            x += adv;
            double along = textPathReverse ? (len - mid) : mid;
            if (along < 0) continue;
            if (along > len) break;
            const double pc = g.percentAtLength(along);
            QPainterPath gp;
            gp.addText(-adv / 2, 0, f, ch);
            QTransform tr;
            tr.translate(g.pointAtPercent(pc).x(), g.pointAtPercent(pc).y());
            tr.rotate(-g.angleAtPercent(pc) + (textPathReverse ? 180 : 0));
            tp.addPath(tr.map(gp));
        }
        return tp;
    }
    for (const auto &l : textLines()) tp.addText(l.baseline, f, l.text);
    return tp;
}

double VObject::lineAdvance() const {
    QFontMetricsF fm(font());
    return fm.lineSpacing() * qMax(0.0, leading) / 100.0;
}

void VObject::fitTextBox() {
    // same metrics as textLines(): kerning/stretch live in the font, leading in lineAdvance()
    QFontMetricsF fm(font());
    const QStringList paras = (text.isEmpty() ? QStringLiteral(" ") : text).split('\n');
    if (textOrientation == 1) {
        const auto lines = textLines();
        if (lines.isEmpty()) { rect.setSize(QSizeF(fm.height() + 2, fm.height() + 2)); return; }
        QRectF b;
        for (const auto &l : lines) {
            const double w = fm.horizontalAdvance(l.text);
            b |= QRectF(l.baseline.x(), l.baseline.y() - fm.ascent(), w, fm.height());
        }
        rect.setSize(QSizeF(qMax(1.0, double(qCeil(b.width()) + 2)), qMax(1.0, double(qCeil(b.height()) + 2))));
        return;
    }
    double w = 0;
    for (const QString &l : paras) w = qMax(w, fm.horizontalAdvance(l.isEmpty() ? QStringLiteral(" ") : l));
    w += paraIndent;
    const double h = (paras.size() - 1) * lineAdvance() + fm.height()
                     + paras.size() * (paraSpaceBefore + paraSpaceAfter);
    rect.setSize(QSizeF(qCeil(w) + 2, qMax(1, qCeil(h))));
}

QString VObject::displayName() const {
    if (!name.isEmpty()) return name;
    if (type == ObjType::Text) {
        QString t = text.left(20);
        t.replace('\n', ' ');
        return t.isEmpty() ? QStringLiteral("Text") : t;
    }
    if (type == ObjType::AutoShape) return autoshape::label(shapeKind);
    if (type == ObjType::Group) return hasMask() ? QStringLiteral("Mask Group") : QStringLiteral("Group");
    if (type == ObjType::Slice && webShape == WebShape::Polygon) return QStringLiteral("Polygon Slice");
    return objTypeLabel(type);
}

int VObject::effectiveInterpolation() const {
    return interpolation >= 0 ? interpolation : coreprefs::interpolation;
}

void VObject::stampInterpolation() {
    if (type == ObjType::Bitmap) interpolation = coreprefs::interpolation;
}

void VObject::ensureInterpolationStamped() {
    if (type == ObjType::Bitmap && interpolation < 0) stampInterpolation();
}
