#include "Stroke.h"
#include <QHash>
#include <QMutex>
#include <QCache>
#include <QFileInfo>
#include <QDateTime>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QLinearGradient>
#include "Render.h"
#include "Object.h"
#include <QJsonArray>
#include <QtMath>
#include <cmath>
#include <random>
#include <functional>
#include <vector>

namespace stroke {
const Params *factory(const QString &key);

namespace {
// 1.0 at the factory ink so stock presets (watercolor.thin, air, …) keep their
// look. A user-edited Ink amount scales relative to that factory value.
double inkScale(const Params &d)
{
    const Params *fac = factory(d.key);
    const int fb = fac ? qMax(1, fac->inkAmount) : 100;
    const int ink = qBound(0, d.inkAmount, 100);
    if (ink == fb) return 1.0;
    return double(ink) / double(fb);
}

// Fireworks-style sensitivity. Identity when pressure=1, speed=1 and the
// Horizontal / Vertical / Random sliders are 0. Faster than 1 shrinks size
// and ink (less dwell); slower grows them. Random is deterministic (seed+dist).
double senseMul(const Sensitivity &s, const PaintInput &in, QPointF pt, double dist, const QRectF &bounds)
{
    if (s.pressure == 0 && s.speed == 0 && s.horizontal == 0 && s.vertical == 0 && s.random == 0)
        return 1.0;
    double k = 1.0;
    if (s.pressure > 0) {
        const double pr = qBound(0.0, in.pressureAt(dist), 1.0);
        k *= 1.0 - (s.pressure / 100.0) * (1.0 - pr);
    }
    if (s.speed > 0) {
        const double dev = qBound(-1.0, in.speedAt(dist) - 1.0, 1.0);
        k *= qMax(0.05, 1.0 - (s.speed / 100.0) * dev);
    }
    if (s.horizontal > 0 && bounds.width() > 1e-6) {
        const double t = qBound(0.0, (pt.x() - bounds.left()) / bounds.width(), 1.0);
        k *= qMax(0.05, 1.0 - (s.horizontal / 100.0) * (t - 0.5) * 2.0);
    }
    if (s.vertical > 0 && bounds.height() > 1e-6) {
        const double t = qBound(0.0, (pt.y() - bounds.top()) / bounds.height(), 1.0);
        k *= qMax(0.05, 1.0 - (s.vertical / 100.0) * (t - 0.5) * 2.0);
    }
    if (s.random > 0) {
        quint32 h = in.seed ^ quint32(qRound(dist * 16)) * 2246822519u;
        h ^= quint32(qRound(pt.x() * 8)) * 2654435761u;
        h ^= quint32(qRound(pt.y() * 8)) * 1597334677u;
        h = (h ^ (h >> 13)) * 1274126177u;
        const double u = double((h >> 8) & 0xffff) / 65535.0;
        k *= qMax(0.05, 1.0 + (s.random / 100.0) * (u - 0.5));
    }
    return qBound(0.05, k, 2.0);
}

double sizeSensePad(const Params &d)
{
    return (d.sizeSens.pressure || d.sizeSens.speed || d.sizeSens.horizontal
            || d.sizeSens.vertical || d.sizeSens.random) ? 2.0 : 1.0;
}

QColor senseSatHue(QColor c, const Params &d, const PaintInput &in, QPointF pt, double dist, const QRectF &bounds)
{
    const double satK = senseMul(d.satSens, in, pt, dist, bounds);
    const double hueK = senseMul(d.hueSens, in, pt, dist, bounds);
    if (satK != 1.0 || hueK != 1.0) {
        int h, s, v, a;
        c.getHsv(&h, &s, &v, &a);
        if (h < 0) h = 0;
        h = (h + int(std::lround((hueK - 1.0) * 90.0)) + 3600) % 360;
        s = qBound(0, int(std::lround(s * satK)), 255);
        v = qBound(0, int(std::lround(v * (0.65 + 0.35 * satK))), 255);
        c.setHsv(h, s, v, a);
    }
    return c;
}

QColor senseColor(QColor c, const Params &d, const PaintInput &in, QPointF pt, double dist, const QRectF &bounds)
{
    const double ink = senseMul(d.inkSens, in, pt, dist, bounds);
    if (ink != 1.0)
        c.setAlphaF(float(qBound(0.0, c.alphaF() * ink, 1.0)));
    return senseSatHue(c, d, in, pt, dist, bounds);
}

double senseAngle(double degrees, const Params &d, const PaintInput &in, QPointF pt, double dist, const QRectF &bounds)
{
    const double k = senseMul(d.angleSens, in, pt, dist, bounds);
    return degrees + (k - 1.0) * 45.0;
}

template <class F>
void walk(const QPainterPath &path, double spacing, F f, double origin = 0);

static std::mt19937 rngAt(quint32 seed, double dist, quint32 salt = 0)
{
    const quint32 d = quint32(std::llround(dist * 1024.0));
    return std::mt19937(seed ^ (d * 2654435761u) ^ (salt * 2246822519u) ^ 0x9e3779b9u);
}

// walk() uses this when the caller does not pass an explicit origin, so a
// suffix paint can keep the same dab grid as a full-path paint.
static thread_local double g_walkOrigin = 0;
struct WalkOriginScope {
    double prev;
    explicit WalkOriginScope(double o) : prev(g_walkOrigin) { g_walkOrigin = o; }
    ~WalkOriginScope() { g_walkOrigin = prev; }
};

QPen qpen(QColor col, double width, Qt::PenCapStyle cap, Qt::PenJoinStyle join);

struct SenseDab {
    QPainter &p;
    bool active;
    SenseDab(QPainter &p, QPointF pt, double sk) : p(p), active(std::abs(sk - 1.0) > 1e-9)
    {
        if (!active) return;
        p.save();
        p.translate(pt);
        p.scale(sk, sk);
        p.translate(-pt);
    }
    ~SenseDab() { if (active) p.restore(); }
};

void drawTintedStamp(QPainter &p, const QImage &src, const QPointF &dest, const QColor &from, const QColor &to)
{
    if (src.isNull() || from.rgba() == to.rgba()) {
        p.drawImage(dest, src);
        return;
    }
    QImage out = src;
    const double fr = qMax(1e-3, from.redF()), fg = qMax(1e-3, from.greenF()), fb = qMax(1e-3, from.blueF());
    const double fa = qMax(1e-3, from.alphaF());
    const double tr = to.redF() / fr, tg = to.greenF() / fg, tb = to.blueF() / fb, ta = to.alphaF() / fa;
    for (int y = 0; y < out.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QRgb px = row[x];
            const int a0 = qAlpha(px);
            if (!a0) continue;
            int r = qBound(0, int(qRed(px) * tr + 0.5), 255);
            int g = qBound(0, int(qGreen(px) * tg + 0.5), 255);
            int b = qBound(0, int(qBlue(px) * tb + 0.5), 255);
            int a = qBound(0, int(a0 * ta + 0.5), 255);
            if (r > a) r = a;
            if (g > a) g = a;
            if (b > a) b = a;
            row[x] = qRgba(r, g, b, a);
        }
    }
    p.drawImage(dest, out);
}

bool senseVaries(const Sensitivity &s, const PaintInput &in)
{
    if (s.horizontal > 0 || s.vertical > 0 || s.random > 0) return true;
    if (s.pressure <= 0 && s.speed <= 0) return false;
    if (in.sampleDist.isEmpty()) {
        // Uniform in.pressure / in.speed: a constant scale, not a ramp.
        return false;
    }
    const double p0 = in.pressureAt(in.sampleDist.first());
    const double s0 = in.speedAt(in.sampleDist.first());
    for (double d : in.sampleDist) {
        if (s.pressure > 0 && std::abs(in.pressureAt(d) - p0) > 1e-6) return true;
        if (s.speed > 0 && std::abs(in.speedAt(d) - s0) > 1e-6) return true;
    }
    return false;
}

void strokeMaybeVary(QPainter &p, const QPainterPath &path, const PaintInput &in,
                     const Params &d, const QColor &c, double w,
                     Qt::PenCapStyle cap, Qt::PenJoinStyle join, const QRectF &bounds)
{
    if (w <= 0) return;
    const WalkOriginScope _walkOrigin(in.pathOrigin);
    const bool ramp = senseVaries(d.sizeSens, in) || senseVaries(d.inkSens, in)
                      || senseVaries(d.satSens, in) || senseVaries(d.hueSens, in);
    if (!ramp) {
        // Constant (including identity at pressure=1, speed=1): one strokePath so
        // factory presets that already store sensitivity values stay pixel-identical.
        const QPointF mid = path.pointAtPercent(0.5);
        const QColor cc = senseColor(c, d, in, mid, 0, bounds);
        const double ww = w * senseMul(d.sizeSens, in, mid, 0, bounds);
        p.strokePath(path, qpen(cc, ww, cap, join));
        return;
    }
    QPointF prev;
    bool has = false;
    walk(path, qMax(0.4, w * 0.2), [&](QPointF pt, QPointF, double dist) {
        const double ww = w * senseMul(d.sizeSens, in, pt, dist, bounds);
        const QColor cc = senseColor(c, d, in, pt, dist, bounds);
        if (has) {
            p.setPen(qpen(cc, ww, cap, join));
            p.setBrush(Qt::NoBrush);
            p.drawLine(prev, pt);
        }
        prev = pt;
        has = true;
    });
}


QString qstr(const char *s) { return QString::fromLatin1(s); }

Params make(const char *key, const char *cat, const char *name, Look look, double a, double b)
{
    Params p;
    p.key = qstr(key);
    p.category = qstr(cat);
    p.name = qstr(name);
    p.look = look;
    p.lookA = a;
    p.lookB = b;
    p.size = 8;
    switch (look) {
    case Look::Pen:
        p.edge = (b > 0) ? 20 : 0;
        p.spacing = 100;
        break;
    case Look::PenRound:
        p.edge = 0;
        break;
    case Look::Hard1:
        p.size = 1;
        p.edge = 0;
        p.spacing = 100;
        break;
    case Look::Soft:
        p.edge = qBound(0, int(a * 100), 100);
        p.inkAmount = 50;
        p.spacing = 25;
        break;
    case Look::Air:
        p.flowRate = 85;
        p.buildUp = true;
        p.edge = 90;
        p.spacing = 20;
        p.inkAmount = qBound(10, int(a * 100), 100);
        p.sizeSens.pressure = 70;
        p.sizeSens.speed = 25;
        p.inkSens.pressure = 50;
        if (p.name == QLatin1String("Textured")) { p.texture = QStringLiteral("Noise"); p.textureAmount = 40; }
        break;
    case Look::Spray:
        p.flowRate = 70;
        p.buildUp = true;
        p.edge = 50;
        p.spacing = 30;
        p.texture = QStringLiteral("Noise");
        p.textureAmount = 40;
        p.sizeSens.pressure = 60;
        p.sizeSens.speed = 30;
        break;
    case Look::Flat:
    case Look::WetCalligraphy:
        p.aspect = a > 0 ? a : 0.2;
        p.angle = p.name == QLatin1String("Quill") ? -62 : -45;
        p.edge = (p.name == QLatin1String("Wet")) ? 20 : (p.name == QLatin1String("Quill") ? 0 : 10);
        p.spacing = qBound(8, int(p.aspect * 80), 40);
        p.sizeSens.pressure = 80;
        p.sizeSens.speed = 30;
        p.inkSens.speed = 20;
        if (p.name == QLatin1String("Bamboo")) { p.texture = QStringLiteral("Grain"); p.textureAmount = 25; }
        if (p.name == QLatin1String("Wet")) p.look = Look::WetCalligraphy;
        break;
    case Look::Grain:
        p.edge = qBound(0, int((1 - a) * 40), 50);
        p.texture = (p.category == QLatin1String("Charcoal") && p.name == QLatin1String("Textured"))
                        ? QStringLiteral("Grain") : p.texture;
        if (p.name == QLatin1String("Textured")) p.textureAmount = 55;
        p.spacing = 30;
        p.sizeSens.pressure = 50;
        p.sizeSens.speed = 20;
        break;
    case Look::Dash:
    case Look::DoubleDash:
        p.dashOn[0] = qMax(1, int(a));
        p.dashOff[0] = qMax(1, int(b));
        break;
    case Look::Bristle:
        p.tips = qMax(2, int(a));
        p.tipSpacing = 20;
        p.edge = 15;
        p.spacing = 20;
        if (p.lookB > 0) { p.texture = QStringLiteral("Grain"); p.textureAmount = qBound(10, int(b * 50), 60); }
        p.sizeSens.pressure = 40;
        break;
    case Look::Dots:
    case Look::Squares:
    case Look::Confetti:
        p.spacing = 40;
        p.colorVar = (look == Look::Confetti) ? ColorVariation::Random : ColorVariation::None;
        break;
    case Look::Squiggle:
        p.tips = qMax(1, int(a));
        p.spacing = 40;
        break;
    case Look::Fur:
        p.spacing = 15;
        p.edge = 10;
        break;
    case Look::Marker:
    case Look::Highlighter:
        p.inkAmount = qBound(20, int(a * 100), 100);
        p.edge = (p.name == QLatin1String("Highlighter")) ? 70 : (p.name == QLatin1String("Thin") ? 15 : 30);
        p.buildUp = true;
        if (p.name == QLatin1String("Highlighter")) p.look = Look::Highlighter;
        p.sizeSens.pressure = 30;
        break;
    case Look::Water:
    case Look::WaterRandom:
        p.inkAmount = qBound(10, int(a * 100), 80);
        p.buildUp = true;
        p.edge = 70;
        p.flowRate = 40;
        p.spacing = 30;
        p.sizeSens.pressure = 50;
        p.inkSens.speed = 40;
        if (p.name == QLatin1String("Random")) p.look = Look::WaterRandom;
        break;
    case Look::Outline:
        p.dashOn[0] = 0;
        break;
    case Look::ThreeD:
    case Look::ThreeDGlow:
        p.edge = (look == Look::ThreeDGlow) ? 60 : 10;
        break;
    case Look::Chameleon:
        p.colorVar = ColorVariation::Hue;
        p.hueSens.random = 80;
        p.hueSens.speed = 40;
        p.spacing = 20;
        p.edge = 20;
        break;
    case Look::FluidSplatter:
    case Look::PaintSplatter:
        p.spacing = look == Look::FluidSplatter ? 50 : 35;
        p.edge = 30;
        break;
    case Look::Toothpaste:
        p.tips = 2;
        p.tipSpacing = 50;
        p.edge = 20;
        p.aspect = 0.7;
        break;
    case Look::ToxicWaste:
        p.colorVar = ColorVariation::Hue;
        p.hueSens.random = 60;
        p.spacing = 40;
        p.edge = 40;
        break;
    case Look::AlienPaint:
        p.edge = 25;
        p.spacing = 25;
        p.flowRate = 30;
        p.colorVar = ColorVariation::Hue;
        p.hueSens.random = 40;
        break;
    case Look::NeonTube:
        p.edge = 80; p.spacing = 16; p.buildUp = true; p.inkAmount = 100;
        break;
    case Look::Gouache:
        p.inkAmount = 100; p.edge = 12; p.spacing = 18; p.square = true;
        break;
    case Look::InkWash:
        p.inkAmount = 55; p.edge = 85; p.flowRate = 50; p.buildUp = true; p.spacing = 22;
        p.sizeSens.pressure = 90; p.inkSens.pressure = 70; p.inkSens.speed = 40;
        break;
    case Look::PixelPencil:
        p.size = 1; p.edge = 0; p.spacing = 100;
        break;
    case Look::RibbonTwist:
        p.aspect = 0.28; p.spacing = 8; p.edge = 8;
        break;
    case Look::Chain:
        p.spacing = 90; p.edge = 0;
        break;
    case Look::Rope:
        p.tips = 3; p.spacing = 12; p.edge = 10;
        break;
    case Look::Stitch:
        p.dashOn[0] = 4; p.dashOff[0] = 6; p.spacing = 100; p.edge = 0;
        break;
    case Look::Glitter:
        p.spacing = 28; p.colorVar = ColorVariation::Random; p.inkAmount = 90;
        break;
    case Look::Smoke:
        p.inkAmount = 35; p.edge = 90; p.flowRate = 60; p.spacing = 16;
        break;
    case Look::Fire:
        p.spacing = 20; p.edge = 40; p.inkAmount = 90;
        break;
    case Look::Rainbow:
        p.colorVar = ColorVariation::Hue; p.spacing = 14; p.edge = 20;
        break;
    case Look::Lightning:
        p.spacing = 10; p.edge = 25; p.inkAmount = 100;
        break;
    case Look::SprayCan:
        p.flowRate = 75; p.buildUp = true; p.spacing = 18; p.edge = 40; p.inkAmount = 80;
        p.inkSens.speed = 70;
        p.sprayLook = 1;
        p.sprayDensity = 55;
        p.spraySpread = 50;
        p.sprayDrips = false;
        break;
    case Look::ImageStamp:
        p.spacing = 90; p.edge = 0;
        break;
    case Look::Cellular:
        p.spacing = 80; p.edge = 0; p.cellSize = 3; p.cellSteps = 2; p.seedDensity = 40;
        p.cellRule = QStringLiteral("B3/S23");
        break;
    default:
        break;
    }
    return p;
}

const QVector<Params> &factoryList()
{
    static const QVector<Params> k = {
        // CS5 menu order: Basic, Air Brush, Calligraphy, Crayon, Felt Tip, Charcoal,
        // Watercolor, Oil, Dashed, Pencil, Random, Unnatural (S6).
        make("basic.hard", "Basic", "Hard Line", Look::Pen, 1, 0),
        make("basic.hardRounded", "Basic", "Hard Rounded", Look::PenRound, 1, 0),
        make("basic.soft", "Basic", "Soft Line", Look::Soft, 0.5, 1),
        make("basic.softRounded", "Basic", "Soft Rounded", Look::Soft, 0.9, 1),

        make("airbrush.basic", "Air Brush", "Basic", Look::Air, 0.18, 0),
        make("airbrush.textured", "Air Brush", "Textured", Look::Spray, 0.8, 0),

        make("calligraphy.bamboo", "Calligraphy", "Bamboo", Look::Flat, 0.3, 1),
        make("calligraphy.basic", "Calligraphy", "Basic", Look::Flat, 0.22, 1),
        make("calligraphy.quill", "Calligraphy", "Quill", Look::Flat, 0.045, 1),
        make("calligraphy.ribbon", "Calligraphy", "Ribbon", Look::Flat, 0.75, 1),
        make("calligraphy.wet", "Calligraphy", "Wet", Look::WetCalligraphy, 0.2, 0.5),

        make("crayon.basic", "Crayon", "Basic", Look::Grain, 0.7, 0),
        make("crayon.rake", "Crayon", "Rake", Look::Bristle, 3, 0.4),
        make("crayon.thick", "Crayon", "Thick", Look::Grain, 0.95, 0),

        make("felttip.dark", "Felt Tip", "Dark Marker", Look::Marker, 0.85, 0),
        make("felttip.light", "Felt Tip", "Light Marker", Look::Marker, 0.55, 0),
        make("felttip.highlighter", "Felt Tip", "Highlighter", Look::Highlighter, 0.4, 1),
        make("felttip.thin", "Felt Tip", "Thin", Look::Marker, 0.9, 0.6),

        make("charcoal.creamy", "Charcoal", "Creamy", Look::Grain, 0.8, 0),
        make("charcoal.pastel", "Charcoal", "Pastel", Look::Grain, 0.55, 0),
        make("charcoal.soft", "Charcoal", "Soft", Look::Grain, 0.4, 1),
        make("charcoal.textured", "Charcoal", "Textured", Look::Grain, 0.3, 0),

        make("watercolor.heavy", "Watercolor", "Heavy", Look::Water, 0.35, 0),
        make("watercolor.thin", "Watercolor", "Thin", Look::Water, 0.15, 0),
        make("watercolor.random", "Watercolor", "Random", Look::WaterRandom, 0.25, 1),

        make("oil.bristle", "Oil", "Bristle", Look::Bristle, 6, 0),
        make("oil.splatter", "Oil", "Splattered Oil", Look::Dots, 0.6, 0),
        make("oil.broadSplatter", "Oil", "Broad Splatter", Look::Dots, 1.2, 0),
        make("oil.strands", "Oil", "Strands", Look::Bristle, 4, 1),
        make("oil.texturedBristle", "Oil", "Textured Bristle", Look::Bristle, 8, 0.5),

        // Six dashed/dotted varieties (S4). English names for the last three are provisional.
        make("dashed.basic", "Dashed", "Basic", Look::Dash, 3, 2),
        make("dashed.dotted", "Dashed", "Dotted", Look::Dash, 1, 2),
        make("dashed.heavy", "Dashed", "Heavy", Look::Dash, 6, 3),
        make("dashed.long", "Dashed", "Long Dash", Look::Dash, 10, 4),
        make("dashed.dotDash", "Dashed", "Dot Dash", Look::Dash, 1, 2),
        make("dashed.double", "Dashed", "Double", Look::DoubleDash, 3, 2),
    };
    return k;
}

const QVector<Params> &allFactory()
{
    static const QVector<Params> kAll = [] {
    QVector<Params> k;
    auto add = [&](Params p) { k.append(std::move(p)); };
    for (const Params &p : factoryList()) add(p);
    // Pencil, Random, Unnatural, and later tips:
    add(make("pencil.1pxHard", "Pencil", "1-Pixel Hard", Look::Hard1, 0, 0));
    add(make("pencil.1pxSoft", "Pencil", "1-Pixel Soft", Look::Pen, 1, 1));
    add(make("pencil.colored", "Pencil", "Colored Pencil", Look::Grain, 0.6, 2));
    add(make("pencil.graphite", "Pencil", "Graphite", Look::Grain, 0.85, 2));

    add(make("random.confetti", "Random", "Confetti", Look::Confetti, 1, 0));
    add(make("random.dots", "Random", "Dots", Look::Dots, 0.8, 0));
    add(make("random.fur", "Random", "Fur", Look::Fur, 1, 0));
    add(make("random.squares", "Random", "Squares", Look::Squares, 0.8, 0));
    add(make("random.yarn", "Random", "Yarn", Look::Squiggle, 3, 0));

    add(make("unnatural.3d", "Unnatural", "3D", Look::ThreeD, 0, 0));
    add(make("unnatural.3dGlow", "Unnatural", "3D Glow", Look::ThreeDGlow, 0, 0));
    add(make("unnatural.chameleon", "Unnatural", "Chameleon", Look::Chameleon, 0, 0));
    add(make("unnatural.fluidSplatter", "Unnatural", "Fluid Splatter", Look::FluidSplatter, 1.4, 0));
    add(make("unnatural.outline", "Unnatural", "Outline", Look::Outline, 0, 0));
    add(make("unnatural.paintSplatter", "Unnatural", "Paint Splatter", Look::PaintSplatter, 0.9, 0));
    add(make("unnatural.squiggle", "Unnatural", "Squiggle", Look::Squiggle, 1, 0));
    add(make("unnatural.toothpaste", "Unnatural", "Toothpaste", Look::Toothpaste, 0, 0));
    add(make("unnatural.toxicWaste", "Unnatural", "Toxic Waste", Look::ToxicWaste, 1.1, 0));
    add(make("unnatural.alienPaint", "Unnatural", "Viscous Alien Paint", Look::AlienPaint, 0, 0));

    // Dot Dash: two On/Off pairs (dot, dash)
    for (Params &p : k) {
        if (p.key == QLatin1String("dashed.dotDash")) {
            p.dashOn[0] = 1; p.dashOff[0] = 2;
            p.dashOn[1] = 6; p.dashOff[1] = 2;
        }
    }
    add(make("lw.neonTube", "Libreworks", "Neon Tube", Look::NeonTube, 0, 0));
    add(make("lw.gouache", "Libreworks", "Gouache", Look::Gouache, 0, 0));
    add(make("lw.inkWash", "Libreworks", "Ink Wash (Sumi-e)", Look::InkWash, 0, 0));
    add(make("lw.pixelPencil", "Libreworks", "Pixel Pencil", Look::PixelPencil, 0, 0));
    add(make("lw.ribbonTwist", "Libreworks", "Ribbon Twist", Look::RibbonTwist, 0, 0));
    add(make("lw.chain", "Libreworks", "Chain", Look::Chain, 0, 0));
    add(make("lw.rope", "Libreworks", "Rope", Look::Rope, 0, 0));
    add(make("lw.stitch", "Libreworks", "Stitch", Look::Stitch, 0, 0));
    add(make("lw.glitter", "Libreworks", "Glitter", Look::Glitter, 0, 0));
    add(make("lw.smoke", "Libreworks", "Smoke", Look::Smoke, 0, 0));
    add(make("lw.fire", "Libreworks", "Fire", Look::Fire, 0, 0));
    add(make("lw.rainbow", "Libreworks", "Rainbow", Look::Rainbow, 0, 0));
    add(make("lw.lightning", "Libreworks", "Lightning", Look::Lightning, 0, 0));
    add(make("lw.sprayCan", "Libreworks", "Spray Can", Look::SprayCan, 0, 0));
    add(make("lw.imageStamp", "Libreworks", "Image Stamp", Look::ImageStamp, 0, 0));
    auto cell = [](const char *key, const char *name, const char *rule, int size, int steps, int dens) {
        Params p = make(key, "Libreworks", name, Look::Cellular, 0, 0);
        p.cellRule = QString::fromLatin1(rule);
        p.cellSize = size;
        p.cellSteps = steps;
        p.seedDensity = dens;
        return p;
    };
    add(cell("lw.cellular", "Cellular", "B3/S23", 3, 3, 62));
    add(cell("lw.cellularHighLife", "Cellular HighLife", "B36/S23", 3, 3, 55));
    add(cell("lw.cellularSeeds", "Cellular Seeds", "B2/S", 3, 1, 55));
    add(cell("lw.cellularDayNight", "Cellular Day & Night", "B3678/S34678", 3, 2, 50));
    add(cell("lw.cellularBrain", "Cellular Brian's Brain", "BB", 3, 1, 48));
    add(cell("lw.cellularWolfram110", "Cellular Wolfram 110", "W110", 2, 1, 50));
    return k;
    }();
    return kAll;
}

template <class F>
void walk(const QPainterPath &path, double spacing, F f, double origin)
{
    spacing = qMax(0.25, spacing);
    if (std::abs(origin) < 1e-15) origin = g_walkOrigin;
    // Place dabs on the same absolute grid as a full-path paint (k * spacing),
    // so a suffix starting at `origin` does not restart the pattern at 0.
    const double firstAbs = std::ceil((origin - 1e-9) / spacing) * spacing;
    const double firstT = qMax(0.0, firstAbs - origin);
    for (const QPolygonF &poly : path.toSubpathPolygons()) {
        if (poly.size() == 1) { f(poly[0], QPointF(1, 0), origin); continue; }
        double carry = 0, dist = origin;
        bool first = true;
        for (int i = 0; i + 1 < poly.size(); ++i) {
            const QPointF a = poly[i], b = poly[i + 1];
            const double len = std::hypot(b.x() - a.x(), b.y() - a.y());
            if (len < 1e-9) continue;
            const QPointF dir = (b - a) / len;
            double t = first ? firstT : spacing - carry;
            first = false;
            for (; t <= len + 1e-9; t += spacing) {
                f(a + dir * t, dir, dist + t);
            }
            carry = len - (t - spacing);
            dist += len;
        }
    }
}

QPolygonF offsetPoly(const QPolygonF &poly, double d, quint32 seed, double jitter, double origin)
{
    QPolygonF out;
    double dist = origin;
    for (int i = 0; i < poly.size(); ++i) {
        QPointF dir = poly[qMin(i + 1, int(poly.size()) - 1)] - poly[qMax(i - 1, 0)];
        double l = std::hypot(dir.x(), dir.y());
        QPointF n = l > 1e-9 ? QPointF(-dir.y() / l, dir.x() / l) : QPointF(0, 0);
        // Per-distance jitter so a mid-path run matches a full-path paint
        // (a sequential drift restarted at every suffix).
        std::mt19937 rng = rngAt(seed, dist, 0x0ff5e7u);
        std::uniform_real_distribution<double> J(-jitter, jitter);
        const double drift = qBound(-jitter * 3, J(rng), jitter * 3);
        out << poly[i] + n * (d + drift);
        if (i + 1 < poly.size())
            dist += std::hypot(poly[i + 1].x() - poly[i].x(), poly[i + 1].y() - poly[i].y());
    }
    return out;
}

QColor varyColor(QColor c, ColorVariation v, double t, std::mt19937 &rng)
{
    std::uniform_real_distribution<double> U(0, 1);
    if (v == ColorVariation::None) return c;
    int h, s, l, a;
    c.getHsl(&h, &s, &l, &a);
    if (h < 0) h = 0;
    switch (v) {
    case ColorVariation::Random:
        return QColor::fromHsv(int(U(rng) * 359), qMax(s, 180), qMax(l, 200), a);
    case ColorVariation::Uniform:
        h = (h + int(t * 0.2)) % 360;
        break;
    case ColorVariation::Complementary:
        h = (h + (int(t / 20) % 2) * 180) % 360;
        break;
    case ColorVariation::Hue:
        h = (h + int((U(rng) - 0.5) * 80 + t * 0.4)) % 360;
        if (h < 0) h += 360;
        break;
    case ColorVariation::Shadow:
        l = qBound(0, l - 40 - int(U(rng) * 30), 255);
        break;
    default:
        break;
    }
    c.setHsl(h, s, l, a);
    return c;
}

QPen qpen(QColor col, double width, Qt::PenCapStyle cap, Qt::PenJoinStyle join)
{
    QPen pn(col, width);
    pn.setCapStyle(cap);
    pn.setJoinStyle(join);
    return pn;
}

QVector<qreal> dashPattern(const Params &p)
{
    QVector<qreal> d;
    for (int i = 0; i < 3; ++i) {
        if (p.dashOn[i] <= 0 && p.dashOff[i] <= 0) continue;
        d << qMax(0.01, double(p.dashOn[i])) << qMax(0.01, double(p.dashOff[i]));
    }
    return d;
}


struct CaRule {
    enum Kind { Life, Brain, Wolfram } kind = Life;
    unsigned born = 0, surv = 0;
    int wolfram = 0;
};

CaRule parseCa(const QString &s)
{
    const QString t = s.trimmed();
    if (t.startsWith(QLatin1String("BB"), Qt::CaseInsensitive)
        || t.contains(QLatin1String("Brain"), Qt::CaseInsensitive))
        return {CaRule::Brain, 0, 0, 0};
    if (t.startsWith(QLatin1Char('W')) || t.startsWith(QLatin1Char('w'))) {
        int n = 0;
        for (QChar c : t.mid(1)) {
            if (!c.isDigit()) continue;
            const int d = c.digitValue();
            if (n > (255 - d) / 10) { n = 255; break; }
            n = n * 10 + d;
        }
        return {CaRule::Wolfram, 0, 0, n};
    }
    CaRule r;
    auto bits = [](const QString &nums) {
        unsigned m = 0;
        for (QChar c : nums) if (c.isDigit() && c.digitValue() <= 8) m |= 1u << c.digitValue();
        return m;
    };
    const int sl = t.indexOf(QLatin1Char('S'), 0, Qt::CaseInsensitive);
    const int bl = t.indexOf(QLatin1Char('B'), 0, Qt::CaseInsensitive);
    QString bpart, spart;
    if (bl >= 0) bpart = t.mid(bl + 1, (sl > bl ? sl : t.size()) - bl - 1);
    if (sl >= 0) spart = t.mid(sl + 1);
    r.born = bits(bpart);
    r.surv = bits(spart);
    if (r.born == 0 && r.surv == 0) { r.born = 1u << 3; r.surv = (1u << 2) | (1u << 3); }
    return r;
}

quint32 cellHash(quint32 seed, int x, int y, int k = 0)
{
    quint32 h = seed ^ quint32(x) * 374761393u ^ quint32(y) * 668265263u ^ quint32(k) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

struct UnderSamp {
    bool on = false, sample = false, follow = false;
    int w = 0, h = 0;
    QVector<quint8> a, r, g, b, edge;
    QVector<quint16> region;
    int idx(int x, int y) const { return y * w + x; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    float alphaAt(double x, double y) const {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        if (!inside(ix, iy)) return 0.f;
        return a[idx(ix, iy)] / 255.f;
    }
    float alphaCell(double cx, double cy, int cs) const {
        if (!on || w <= 0) return 0.f;
        const int x0 = qBound(0, int(std::floor(cx)), w - 1);
        const int y0 = qBound(0, int(std::floor(cy)), h - 1);
        const int x1 = qBound(0, int(std::ceil(cx + cs)) - 1, w - 1);
        const int y1 = qBound(0, int(std::ceil(cy + cs)) - 1, h - 1);
        if (x1 < x0 || y1 < y0) return alphaAt(cx, cy);
        int sum = 0, n = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) { sum += a[idx(x, y)]; ++n; }
        return n ? sum / (255.f * n) : 0.f;
    }
    float edgeAt(double x, double y) const {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        if (!inside(ix, iy) || edge.isEmpty()) return 0.f;
        return edge[idx(ix, iy)] / 255.f;
    }
    quint16 regionAt(double x, double y) const {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        if (!inside(ix, iy) || region.isEmpty()) return 0;
        return region[idx(ix, iy)];
    }
    void accumColor(double cx, double cy, int cs, double *sr, double *sg, double *sb, double *sa) const {
        *sr = *sg = *sb = *sa = 0;
        if (!on || w <= 0) return;
        const int x0 = qBound(0, int(std::floor(cx)), w - 1);
        const int y0 = qBound(0, int(std::floor(cy)), h - 1);
        const int x1 = qBound(0, int(std::ceil(cx + cs)) - 1, w - 1);
        const int y1 = qBound(0, int(std::ceil(cy + cs)) - 1, h - 1);
        int n = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const int i = idx(x, y);
                const float aa = a[i] / 255.f;
                *sr += (r[i] / 255.f) * aa;
                *sg += (g[i] / 255.f) * aa;
                *sb += (b[i] / 255.f) * aa;
                *sa += aa;
                ++n;
            }
        }
        if (n && *sa > 1e-6) { *sr /= *sa; *sg /= *sa; *sb /= *sa; *sa /= n; }
        else { *sr = *sg = *sb = *sa = 0; }
    }
};

// Per-thread: live-stroke worker and the UI thread may both call stroke::paint.
thread_local qint64 g_underKey = -1;
thread_local int g_underFlags = -1;
thread_local UnderSamp g_under;

void clearUnderCache()
{
    g_under = UnderSamp();
    g_underKey = -1;
    g_underFlags = -1;
}

UnderSamp makeUnder(const PaintInput &in)
{
    UnderSamp u;
    if ((!in.params.cellSampleUnder && !in.params.cellFollowEdges) || in.underImage.isNull())
        return u;
    // m_base is constant for a stroke; reuse the planes instead of rebuilding them
    // on every segment (measured 24–86× on a 4000×3000 canvas). Released in
    // brush::Stroke::end() via releaseUnderSampleCache().
    const qint64 key = in.underImage.cacheKey();
    const int flags = int(in.params.cellSampleUnder) | (int(in.params.cellFollowEdges) << 1);
    if (key == g_underKey && flags == g_underFlags)
        return g_under;
    const QImage img = in.underImage.format() == QImage::Format_ARGB32_Premultiplied
                       ? in.underImage
                       : in.underImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    u.w = img.width();
    u.h = img.height();
    if (u.w <= 0 || u.h <= 0) return u;
    const int n = u.w * u.h;
    u.a.resize(n); u.r.resize(n); u.g.resize(n); u.b.resize(n);
    bool any = false;
    for (int y = 0; y < u.h; ++y) {
        const auto *sl = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < u.w; ++x) {
            const QRgb px = sl[x];
            const int A = qAlpha(px);
            const int i = y * u.w + x;
            u.a[i] = quint8(A);
            if (A) {
                any = true;
                // unpremultiply so sampling is in straight colour
                u.r[i] = quint8(qRed(px) * 255 / A);
                u.g[i] = quint8(qGreen(px) * 255 / A);
                u.b[i] = quint8(qBlue(px) * 255 / A);
            } else {
                u.r[i] = u.g[i] = u.b[i] = 0;
            }
        }
    }
    if (!any) {
        g_under = UnderSamp();
        g_underKey = key;
        g_underFlags = flags;
        return g_under;   // fully empty: identical to the default path
    }
    u.on = true;
    u.sample = in.params.cellSampleUnder;
    u.follow = in.params.cellFollowEdges;
    if (!u.follow) {
        g_under = u;
        g_underKey = key;
        g_underFlags = flags;
        return g_under;
    }
    u.edge.resize(n);
    u.region.resize(n);
    QVector<float> lum(n);
    for (int i = 0; i < n; ++i) {
        if (u.a[i] < 8) { lum[i] = 0; u.region[i] = 0; continue; }
        lum[i] = (0.299f * u.r[i] + 0.587f * u.g[i] + 0.114f * u.b[i]) * (u.a[i] / 255.f);
        u.region[i] = quint16(((u.r[i] >> 5) << 6) | ((u.g[i] >> 5) << 3) | (u.b[i] >> 5));
        if (!u.region[i]) u.region[i] = 1; // reserve 0 for empty
    }
    float maxm = 1e-6f;
    QVector<float> mag(n, 0.f);
    for (int y = 1; y < u.h - 1; ++y) {
        for (int x = 1; x < u.w - 1; ++x) {
            const int i = y * u.w + x;
            const float gx = -lum[i - u.w - 1] + lum[i - u.w + 1]
                           - 2 * lum[i - 1] + 2 * lum[i + 1]
                           - lum[i + u.w - 1] + lum[i + u.w + 1];
            const float gy = -lum[i - u.w - 1] - 2 * lum[i - u.w] - lum[i - u.w + 1]
                           + lum[i + u.w - 1] + 2 * lum[i + u.w] + lum[i + u.w + 1];
            const float m = std::hypot(gx, gy);
            mag[i] = m;
            if (m > maxm) maxm = m;
        }
    }
    for (int i = 0; i < n; ++i)
        u.edge[i] = quint8(qBound(0, int(mag[i] / maxm * 255.f + 0.5f), 255));
    g_under = u;
    g_underKey = key;
    g_underFlags = flags;
    return g_under;
}

QColor blendCellColor(const QColor &generated, const QColor &brush, const UnderSamp &u,
                      double cx, double cy, int cs)
{
    if (!u.on || !u.sample) return generated;
    double sr, sg, sb, sa;
    u.accumColor(cx, cy, cs, &sr, &sg, &sb, &sa);
    if (sa <= 1e-6) return generated;
    // Tint the sampled colour with the brush, then fade toward the generated
    // pattern by the inverse of the under alpha so empty pixels stay unchanged.
    const double tint = 0.35;
    const double mr = sr * (1.0 - tint) + brush.redF() * tint;
    const double mg = sg * (1.0 - tint) + brush.greenF() * tint;
    const double mb = sb * (1.0 - tint) + brush.blueF() * tint;
    const double ua = qBound(0.0, sa, 1.0);
    QColor out;
    out.setRgbF(
        generated.redF() * (1.0 - ua) + mr * ua,
        generated.greenF() * (1.0 - ua) + mg * ua,
        generated.blueF() * (1.0 - ua) + mb * ua,
        generated.alphaF());
    return out;
}

int followDensity(int baseDens, const UnderSamp &u, double cx, double cy, int cs)
{
    if (!u.on || !u.follow) return baseDens;
    const float ua = u.alphaCell(cx, cy, cs);
    if (ua <= 0.f) return baseDens;   // empty: identical to the default threshold
    const float e = u.edgeAt(cx + cs * 0.5, cy + cs * 0.5);
    const int edged = qBound(0, int(baseDens * (0.25 + 1.5 * e) + 0.5), 100);
    return qBound(0, int(baseDens * (1.f - ua) + edged * ua + 0.5f), 100);
}

void paintCellular(QPainter &p, const QPainterPath &path, const PaintInput &in)
{
    const Params &d = in.params;
    const double w = qMax(1.0, in.width);
    QColor c = in.color;
    const double inkK = inkScale(d);
    if (inkK != 1.0)
        c.setAlphaF(float(qBound(0.0, c.alphaF() * inkK, 1.0)));
    const CaRule rule = parseCa(d.cellRule);
    int cs = qBound(1, d.cellSize, 32);
    const int steps = qBound(0, d.cellSteps, 16);
    const int dens = qBound(0, d.seedDensity, 100);
    QColor ageCol = d.cellAgeColor.isValid() ? d.cellAgeColor
                    : (c.lightness() > 180 ? c.darker(150) : c.lighter(180));
    if (!d.cellAgeColor.isValid() && c.saturation() > 20) {
        int h, s, l, a; c.getHsl(&h, &s, &l, &a);
        ageCol.setHsl((h + 40) % 360, s, qBound(40, l + 50, 230), a);
    }
    const UnderSamp under = makeUnder(in);

    if (rule.kind == CaRule::Wolfram) {
        // 1D CA: space is across the stroke, time evolves along the path.
        QVector<QPointF> samples;
        QVector<QPointF> normals;
        walk(path, qMax(0.8, double(cs)), [&](QPointF pt, QPointF dir, double) {
            samples << pt;
            const double l = std::hypot(dir.x(), dir.y());
            normals << (l > 1e-9 ? QPointF(-dir.y() / l, dir.x() / l) : QPointF(0, -1));
        });
        if (samples.size() < 2) return;
        const int cols = qMax(3, int(std::ceil(w / cs)) | 1); // odd so there is a centre cell
        const int cap = 4096; // wide strokes must keep their full width
        const int n = qMin(cols, cap);
        QVector<quint8> cur(n, 0), nxt(n, 0);
        QVector<quint8> birth(n, 0);
        for (int j = 0; j < n; ++j) {
            const QPointF pt0 = samples[0] + normals[0] * ((j - (n - 1) / 2.0) * cs);
            const int d0 = followDensity(dens, under, pt0.x() - cs * 0.48, pt0.y() - cs * 0.48, cs);
            if (int(cellHash(in.seed, j, 0) % 100) < d0 || j == n / 2) { cur[j] = 1; birth[j] = 0; }
        }
        auto drawRow = [&](int i, const QVector<quint8> &row, const QVector<quint8> &ages) {
            for (int j = 0; j < n; ++j) {
                if (!row[j]) continue;
                const double t = qBound(0.0, ages[j] / 10.0, 1.0);
                QColor col = QColor::fromRgbF(
                    c.redF() * (1 - t) + ageCol.redF() * t,
                    c.greenF() * (1 - t) + ageCol.greenF() * t,
                    c.blueF() * (1 - t) + ageCol.blueF() * t,
                    c.alphaF());
                const QPointF pt = samples[i] + normals[i] * ((j - (n - 1) / 2.0) * cs);
                col = blendCellColor(col, c, under, pt.x() - cs * 0.48, pt.y() - cs * 0.48, cs);
                p.setBrush(col);
                p.setPen(Qt::NoPen);
                p.drawRect(QRectF(pt.x() - cs * 0.48, pt.y() - cs * 0.48, cs * 0.96, cs * 0.96));
            }
        };
        drawRow(0, cur, birth);
        for (int i = 1; i < samples.size(); ++i) {
            for (int j = 0; j < n; ++j) {
                const int l = cur[(j + n - 1) % n], m = cur[j], rgt = cur[(j + 1) % n];
                const int idx = (l << 2) | (m << 1) | rgt;
                nxt[j] = (rule.wolfram >> idx) & 1;
                birth[j] = nxt[j] ? (cur[j] ? quint8(qMin(255, birth[j] + 1)) : 0) : 0;
            }
            cur.swap(nxt);
            drawRow(i, cur, birth);
        }
        return;
    }

    // 2D CA on a canvas-aligned grid clipped to the stroke coverage.
    QPainterPathStroker st;
    st.setWidth(qMax(1.0, w));
    st.setCapStyle(Qt::RoundCap);
    st.setJoinStyle(Qt::RoundJoin);
    const QPainterPath cover = st.createStroke(path);
    QRectF br = cover.boundingRect().adjusted(-cs, -cs, cs, cs);
    if (br.isEmpty()) return;
    if (in.cellSizeLock > 0 && in.gridLimit.isValid() && !in.gridLimit.isEmpty()) {
        const int halo = (qMax(0, in.params.cellSteps) + 1) * qMax(1, in.cellSizeLock);
        br = br.intersected(QRectF(in.gridLimit).adjusted(-halo, -halo, halo, halo));
        if (br.isEmpty()) return;
    }
    const int MAXN = 256 * 256;
    const qint64 LOCK_CAP = 1024 * 1024; // 1M cells (~5 MB of CA planes)
    int gw, gh;
    QPointF origin;
    if (in.cellSizeLock > 0) {
        // Live bitmap stroke: keep the tip's cell size and a world-aligned origin so cells
        // do not jump as the path grows. Clip to gridLimit + halo (when set) and refuse
        // more than LOCK_CAP cells so gw*gh cannot overflow or allocate gigabytes.
        cs = in.cellSizeLock;
        origin = QPointF(std::floor(br.left() / cs) * cs, std::floor(br.top() / cs) * cs);
        qint64 gw64 = qMax(qint64(1), qint64(std::ceil((br.right() - origin.x()) / cs)));
        qint64 gh64 = qMax(qint64(1), qint64(std::ceil((br.bottom() - origin.y()) / cs)));
        if (gw64 > INT_MAX || gh64 > INT_MAX || gw64 * gh64 > LOCK_CAP) {
            const qint64 n = qMin(gw64 * gh64, qint64(INT_MAX) * 4);
            const double scale = std::sqrt(double(n) / double(LOCK_CAP));
            cs = qMax(cs, int(std::ceil(cs * scale)));
            origin = QPointF(std::floor(br.left() / cs) * cs, std::floor(br.top() / cs) * cs);
            gw64 = qMax(qint64(1), qint64(std::ceil((br.right() - origin.x()) / cs)));
            gh64 = qMax(qint64(1), qint64(std::ceil((br.bottom() - origin.y()) / cs)));
            gw64 = qMin(gw64, qint64(INT_MAX));
            gh64 = qMin(gh64, qint64(INT_MAX));
            if (gw64 * gh64 > LOCK_CAP) {
                gh64 = qMax(qint64(1), LOCK_CAP / qMax(qint64(1), gw64));
            }
        }
        gw = int(gw64);
        gh = int(gh64);
    } else {
        gw = qMax(1, int(std::ceil(br.width() / cs)));
        gh = qMax(1, int(std::ceil(br.height() / cs)));
        // Coarsen the cell size so long paths stay fully covered; never clip the grid.
        while (qint64(gw) * gh > MAXN && cs < 256) {
            ++cs;
            gw = qMax(1, int(std::ceil(br.width() / cs)));
            gh = qMax(1, int(std::ceil(br.height() / cs)));
        }
        if (qint64(gw) * gh > MAXN) {
            const double scale = std::sqrt(double(qint64(gw) * gh) / MAXN);
            cs = qMax(cs, int(std::ceil(cs * scale)));
            gw = qMax(1, int(std::ceil(br.width() / cs)));
            gh = qMax(1, int(std::ceil(br.height() / cs)));
        }
        origin = br.topLeft();
    }
    const qint64 n64 = qint64(gw) * qint64(gh);
    if (n64 <= 0 || n64 > INT_MAX) return;
    const int N = int(n64);
    std::vector<uint8_t> inCover(N, 0);
    // Spatial bucketing: mark cells from path samples instead of intersects() per cell.
    const double reach0 = w / 2 + cs;
    walk(path, qMax(double(cs) * 0.5, 1.0), [&](QPointF pt, QPointF, double) {
        const int x0 = qBound(0, int(std::floor((pt.x() - reach0 - origin.x()) / cs)), gw - 1);
        const int x1 = qBound(0, int(std::ceil((pt.x() + reach0 - origin.x()) / cs)), gw - 1);
        const int y0 = qBound(0, int(std::floor((pt.y() - reach0 - origin.y()) / cs)), gh - 1);
        const int y1 = qBound(0, int(std::ceil((pt.y() + reach0 - origin.y()) / cs)), gh - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const QPointF cc(origin.x() + (x + 0.5) * cs, origin.y() + (y + 0.5) * cs);
                if (QLineF(pt, cc).length() <= reach0) inCover[y * gw + x] = 1;
            }
        }
    });

    std::vector<uint8_t> mask(N, 0), state(N, 0), next(N, 0), age(N, 0);
    // Bitset-like packed presence for neighbour tests is overkill at 64k; byte grids stay in L1.
    auto idx = [gw](int x, int y) { return y * gw + x; };
    auto blockedByEdge = [&](int x, int y, int xx, int yy) {
        if (!under.on || !under.follow) return false;
        const QPointF a(origin.x() + (x + 0.5) * cs, origin.y() + (y + 0.5) * cs);
        const QPointF b(origin.x() + (xx + 0.5) * cs, origin.y() + (yy + 0.5) * cs);
        const float ua = under.alphaAt(a.x(), a.y()), ub = under.alphaAt(b.x(), b.y());
        if (ua < 0.2f || ub < 0.2f) return false;   // empty side keeps the default neighbourhood
        const quint16 ra = under.regionAt(a.x(), a.y()), rb = under.regionAt(b.x(), b.y());
        if (ra && rb && ra != rb) return true;
        const QPointF mid = (a + b) * 0.5;
        return under.edgeAt(mid.x(), mid.y()) > 0.45f;
    };
    auto countOn = [&](int x, int y) {
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= gw || yy >= gh) continue;
            if (blockedByEdge(x, y, xx, yy)) continue;
            if (state[idx(xx, yy)] == 1) ++n;
        }
        return n;
    };

    // Dabs along the path unlock and seed cells (from neighbouring live cells + noise).
    // The rule then runs N steps once on the finished mask so still-lifes stay intact
    // and later dabs cannot burn earlier cells out.
    const double dabStep = qMax(double(cs), w * qMax(0.25, d.spacing / 100.0));
    walk(path, dabStep, [&](QPointF pt, QPointF, double) {
        const double reach = w / 2 + cs;
        const int x0 = qBound(0, int(std::floor((pt.x() - reach - origin.x()) / cs)), gw - 1);
        const int x1 = qBound(0, int(std::ceil((pt.x() + reach - origin.x()) / cs)), gw - 1);
        const int y0 = qBound(0, int(std::floor((pt.y() - reach - origin.y()) / cs)), gh - 1);
        const int y1 = qBound(0, int(std::ceil((pt.y() + reach - origin.y()) / cs)), gh - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const int i = idx(x, y);
                if (!inCover[i] || mask[i]) continue;
                const QPointF cc(origin.x() + (x + 0.5) * cs, origin.y() + (y + 0.5) * cs);
                if (QLineF(pt, cc).length() > reach) continue;
                mask[i] = 1;
                int neigh = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= gw || yy >= gh) continue;
                    const int j = idx(xx, yy);
                    if (mask[j] && state[j] == 1) ++neigh;
                }
                // Isolated 2×2 still-lifes on a 4-cell lattice (Life / Seeds / HighLife).
                // Light noise only — flooding from every neighbour fills the mask and
                // B3/S23 then dies of overpopulation. Packed rules (Day & Night) want
                // a dense labyrinth, so they keep a higher seed rate.
                const int survN = __builtin_popcount(rule.surv);
                const bool packed = rule.kind == CaRule::Life && survN >= 4;
                const bool block = (rule.kind == CaRule::Life
                                    && (x % 4 == 0) && (y % 4 == 0)
                                    && int(cellHash(in.seed, x / 4, y / 4, 3) % 100) < qMax(dens, 55));
                const int noiseP0 = packed ? qMax(dens, 55)
                                   : (rule.kind == CaRule::Life) ? qBound(8, dens / 4, 18) : dens;
                const int noiseP = followDensity(noiseP0, under, origin.x() + x * cs, origin.y() + y * cs, cs);
                const bool fromNoise = int(cellHash(in.seed, x, y, 2) % 100) < noiseP;
                const bool fromN = packed ? (neigh > 0)
                                   : (rule.kind == CaRule::Brain
                                      && neigh > 0
                                      && int(cellHash(in.seed, x, y, 4) % 100) < 40);
                if (fromN || fromNoise) { state[i] = 1; age[i] = 0; }
                if (block) {
                    for (int dy = 0; dy <= 1; ++dy) for (int dx = 0; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= gw || yy >= gh) continue;
                        const int j = idx(xx, yy);
                        if (!inCover[j]) continue;
                        mask[j] = 1;
                        state[j] = 1;
                        age[j] = 0;
                    }
                }
            }
        }
    });

    if (steps > 0) {
        for (int s = 0; s < steps; ++s) {
            for (int y = 0; y < gh; ++y) {
                for (int x = 0; x < gw; ++x) {
                    const int i = idx(x, y);
                    if (!mask[i]) { next[i] = 0; continue; }
                    const int n = countOn(x, y);
                    if (rule.kind == CaRule::Brain) {
                        if (state[i] == 0) next[i] = (n == 2) ? 1 : 0;
                        else if (state[i] == 1) next[i] = 2;
                        else next[i] = 0;
                    } else {
                        const unsigned bit = 1u << n;
                        const bool live = state[i] == 1;
                        next[i] = (live && (rule.surv & bit)) || (!live && (rule.born & bit)) ? 1 : 0;
                    }
                }
            }
            for (int i = 0; i < N; ++i) {
                if (!mask[i]) continue;
                if (next[i] == 1) age[i] = (state[i] == 1) ? quint8(qMin(255, age[i] + 1)) : 0;
                else if (next[i] == 0) age[i] = 0;
                state[i] = next[i];
            }
        }
    }

    p.setPen(Qt::NoPen);
    for (int y = 0; y < gh; ++y) {
        for (int x = 0; x < gw; ++x) {
            const int i = idx(x, y);
            if (!mask[i] || state[i] == 0) continue;
            const double t = state[i] == 2 ? 0.55 : qBound(0.0, age[i] / 8.0, 1.0);
            QColor col = QColor::fromRgbF(
                c.redF() * (1 - t) + ageCol.redF() * t,
                c.greenF() * (1 - t) + ageCol.greenF() * t,
                c.blueF() * (1 - t) + ageCol.blueF() * t,
                c.alphaF() * (state[i] == 2 ? 0.55 : 1.0));
            col = blendCellColor(col, c, under, origin.x() + x * cs, origin.y() + y * cs, cs);
            p.setBrush(col);
            p.drawRect(QRectF(origin.x() + x * cs, origin.y() + y * cs, cs, cs));
        }
    }
}

} // namespace

bool Params::operator==(const Params &o) const
{
    return key == o.key && category == o.category && name == o.name
        && inkAmount == o.inkAmount && spacing == o.spacing && flowRate == o.flowRate
        && buildUp == o.buildUp && texture == o.texture && textureAmount == o.textureAmount
        && edgeTexture == o.edgeTexture && edgeEffect == o.edgeEffect
        && tips == o.tips && tipSpacing == o.tipSpacing && colorVar == o.colorVar
        && dashOn[0] == o.dashOn[0] && dashOn[1] == o.dashOn[1] && dashOn[2] == o.dashOn[2]
        && dashOff[0] == o.dashOff[0] && dashOff[1] == o.dashOff[1] && dashOff[2] == o.dashOff[2]
        && square == o.square && size == o.size && edge == o.edge && aspect == o.aspect && angle == o.angle
        && sizeSens == o.sizeSens && inkSens == o.inkSens && satSens == o.satSens
        && hueSens == o.hueSens && angleSens == o.angleSens
        && look == o.look && lookA == o.lookA && lookB == o.lookB && stampSource == o.stampSource
        && cellRule == o.cellRule && cellSize == o.cellSize && cellSteps == o.cellSteps
        && seedDensity == o.seedDensity && cellAgeColor == o.cellAgeColor
        && cellSampleUnder == o.cellSampleUnder && cellFollowEdges == o.cellFollowEdges
        && sprayLook == o.sprayLook && sprayDensity == o.sprayDensity
        && spraySpread == o.spraySpread && sprayDrips == o.sprayDrips;
}

const QVector<Preset> &presets()
{
    static const QVector<Preset> v = [] {
        QVector<Preset> list;
        for (const Params &p : allFactory()) list.append({p.key, p.category, p.name});
        return list;
    }();
    return v;
}

QStringList categories()
{
    QStringList l;
    for (const auto &p : presets()) if (!l.contains(p.category)) l << p.category;
    return l;
}

QStringList namesInCategory(const QString &category)
{
    QStringList l;
    for (const auto &p : presets()) if (p.category == category) l << p.name;
    return l;
}

QString keyFor(const QString &category, const QString &name)
{
    for (const auto &p : presets()) if (p.category == category && p.name == name) return p.key;
    return QStringLiteral("basic.hard");
}

QString canonicalKey(const QString &keyOrAlias)
{
    static const QHash<QString, QString> alias = {
        {QStringLiteral("oil.splattered"), QStringLiteral("oil.splatter")},
    };
    const QString mapped = alias.value(keyOrAlias, keyOrAlias);
    for (const auto &p : presets()) if (p.key == mapped) return p.key;
    for (const auto &p : presets()) if (p.category == keyOrAlias) return p.key;
    return QStringLiteral("basic.hard");
}

QString label(const QString &key)
{
    const Params *p = factory(key);
    if (!p) return key;
    return p->category + QStringLiteral(": ") + p->name;
}

const Params *factory(const QString &key)
{
    const QString k = [&] {
        for (const auto &p : allFactory()) if (p.key == key) return key;
        return canonicalKey(key);
    }();
    for (const auto &p : allFactory()) if (p.key == k) return &p;
    return &allFactory().first();
}

bool usesPathEngine(Look look)
{
    switch (look) {
    case Look::Pen:
    case Look::PenRound:
    case Look::Hard1:
    case Look::Soft:
    case Look::Air:
        return false;
    default:
        return true;
    }
}

bool pathGlobalLook(Look look)
{
    switch (look) {
    case Look::Smoke:
    case Look::Cellular:
    case Look::Fire:
    case Look::Lightning:
        return true;
    default:
        return false;
    }
}

bool pathGlobalLook(const Params &p)
{
    if (pathGlobalLook(p.look)) return true;
    // Quill taper is a function of the finished path length, so earlier
    // pixels change as the stroke grows.
    if (p.look == Look::Flat && p.name.contains(QLatin1String("Quill")))
        return true;
    if (p.look == Look::DoubleDash)
        return true;
    return false;
}

bool usesEdgeSoftness(Look look)
{
    switch (look) {
    case Look::Pen:
    case Look::PenRound:
    case Look::Soft:
    case Look::Air:
        return true;
    default:
        return false;
    }
}

static const Option kAllOptions[] = {
    Option::InkAmount, Option::Spacing, Option::Flow, Option::BuildUp, Option::Texture,
    Option::Tips, Option::TipSpacing, Option::ColorVariation, Option::Dash, Option::Square,
    Option::Size, Option::Edge, Option::Aspect, Option::Angle,
    Option::SizePressure, Option::SizeSpeed, Option::InkPressure, Option::InkSpeed,
    Option::SaturationSens, Option::HueSens, Option::AngleSens,
    Option::EdgeTexture, Option::EdgeEffect,
    Option::CellRule, Option::CellSize, Option::CellSteps, Option::SeedDensity, Option::CellAgeColor,
    Option::CellSampleUnder, Option::CellFollowEdges,
    Option::StampSource,
    Option::SprayDensity, Option::SpraySpread, Option::SprayDrips
};

const QVector<Option> &allOptions()
{
    static const QVector<Option> k = [] {
        QVector<Option> v;
        for (Option o : kAllOptions) v.append(o);
        return v;
    }();
    return k;
}

QString optionLabel(Option opt)
{
    switch (opt) {
    case Option::InkAmount: return QStringLiteral("Ink amount");
    case Option::Spacing: return QStringLiteral("Spacing");
    case Option::Flow: return QStringLiteral("Flow");
    case Option::BuildUp: return QStringLiteral("Build-up");
    case Option::Texture: return QStringLiteral("Texture");
    case Option::Tips: return QStringLiteral("Tips");
    case Option::TipSpacing: return QStringLiteral("Tip spacing");
    case Option::ColorVariation: return QStringLiteral("Color variation");
    case Option::Dash: return QStringLiteral("Dash");
    case Option::Square: return QStringLiteral("Shape (round/square)");
    case Option::Size: return QStringLiteral("Size");
    case Option::Edge: return QStringLiteral("Edge");
    case Option::Aspect: return QStringLiteral("Shape roundness");
    case Option::Angle: return QStringLiteral("Shape angle");
    case Option::SizePressure: return QStringLiteral("Size pressure");
    case Option::SizeSpeed: return QStringLiteral("Size speed");
    case Option::InkPressure: return QStringLiteral("Ink pressure");
    case Option::InkSpeed: return QStringLiteral("Ink speed");
    case Option::SaturationSens: return QStringLiteral("Saturation sensitivity");
    case Option::HueSens: return QStringLiteral("Hue sensitivity");
    case Option::AngleSens: return QStringLiteral("Angle sensitivity");
    case Option::EdgeTexture: return QStringLiteral("Edge texture");
    case Option::EdgeEffect: return QStringLiteral("Edge effect");
    case Option::CellRule: return QStringLiteral("Cellular rule");
    case Option::CellSize: return QStringLiteral("Cellular size");
    case Option::CellSteps: return QStringLiteral("Cellular steps");
    case Option::SeedDensity: return QStringLiteral("Cellular seed density");
    case Option::CellAgeColor: return QStringLiteral("Cellular age color");
    case Option::CellSampleUnder: return QStringLiteral("Cellular sample under");
    case Option::CellFollowEdges: return QStringLiteral("Cellular follow edges");
    case Option::StampSource: return QStringLiteral("Stamp source");
    case Option::SprayDensity: return QStringLiteral("Density");
    case Option::SpraySpread: return QStringLiteral("Spread");
    case Option::SprayDrips: return QStringLiteral("Drips");
    }
    return QString();
}

bool optionApplies(Look look, Option opt)
{
    switch (opt) {
    case Option::Size:
        return look != Look::PixelPencil;
    case Option::InkAmount:
    case Option::Texture:
        return true;
    case Option::Spacing:
        switch (look) {
        case Look::Dash: case Look::DoubleDash: case Look::Outline:
        case Look::ThreeD: case Look::ThreeDGlow:
        case Look::Bristle:
        case Look::PixelPencil:
        case Look::Toothpaste:
        case Look::NeonTube:
            return false;
        default:
            return true;
        }
    case Option::Flow:
    case Option::BuildUp:
        if (look == Look::SprayCan) return true;
        return !usesPathEngine(look);
    case Option::Tips:
        return look == Look::Bristle || look == Look::Squiggle || look == Look::Rope;
    case Option::TipSpacing:
        return look == Look::Bristle;
    case Option::ColorVariation:
        return look == Look::Dots || look == Look::Squares || look == Look::Confetti
            || look == Look::FluidSplatter || look == Look::PaintSplatter;
    case Option::Dash:
        return look == Look::Dash || look == Look::DoubleDash;
    case Option::Square:
        return look == Look::Gouache || !usesPathEngine(look);
    case Option::Edge:
        return usesEdgeSoftness(look);
    case Option::Aspect:
    case Option::Angle:
        return look == Look::Flat || look == Look::WetCalligraphy || !usesPathEngine(look);
    case Option::SizePressure:
    case Option::SizeSpeed:
        // Pixel Pencil is a 1 px tip; size sensitivity would do nothing visible.
        // Cellular is a path-global CA grid, not a tapered stroke.
        return usesPathEngine(look) && look != Look::PixelPencil && look != Look::Cellular;
    case Option::InkPressure:
    case Option::InkSpeed:
    case Option::SaturationSens:
    case Option::HueSens:
        // Dab-engine tips (Pen, Soft, Air, …) keep using the Brush PI tablet
        // pressure toggles. Path-engine tips read the Sensitivity tab.
        return usesPathEngine(look) && look != Look::Cellular;
    case Option::AngleSens:
        return look == Look::Flat || look == Look::WetCalligraphy || look == Look::RibbonTwist;
    case Option::EdgeTexture:
    case Option::EdgeEffect:
        return false;
    case Option::CellRule:
    case Option::CellSize:
    case Option::CellSteps:
    case Option::SeedDensity:
    case Option::CellAgeColor:
    case Option::CellSampleUnder:
    case Option::CellFollowEdges:
        return look == Look::Cellular;
    case Option::StampSource:
        return look == Look::ImageStamp;
    case Option::SprayDensity:
    case Option::SpraySpread:
    case Option::SprayDrips:
        return look == Look::SprayCan;
    }
    return false;
}

bool optionApplies(const Params &p, Option opt)
{
    if (p.look == Look::Cellular) {
        const QString rule = p.cellRule.isEmpty() ? QStringLiteral("B3/S23") : p.cellRule;
        const bool wolfram = rule.startsWith(QLatin1Char('W')) || rule.startsWith(QLatin1Char('w'));
        const bool seeds = rule.contains(QLatin1String("Seeds"), Qt::CaseInsensitive)
                           || rule == QLatin1String("B2/S");
        if (opt == Option::CellAgeColor && seeds) return false;
        if ((opt == Option::Spacing || opt == Option::CellSteps) && wolfram) return false;
    }
    if (opt == Option::Angle && !usesPathEngine(p.look)
        && !(p.aspect > 0.0 && p.aspect < 0.999))
        return false;
    return optionApplies(p.look, opt);
}

double spaceStep(const Params &d, double base)
{
    const Params *fac = factory(d.key);
    const int fb = fac ? qMax(1, fac->spacing) : 25;
    const int sp = d.spacing > 0 ? d.spacing : fb;
    if (sp == fb) return qMax(0.25, base);
    return qMax(0.25, base * (double(sp) / double(fb)));
}

Params paramsFor(const QString &key)
{
    return *factory(key);
}

double paintedRadiusUnsens(const Params &p, double width);

double paintedRadius(const Params &p, double width)
{
    return paintedRadiusUnsens(p, width) * sizeSensePad(p);
}

double paintedRadiusUnsens(const Params &p, double width)
{
    const double w = qMax(0.5, width);
    const double half = w / 2.0;
    const double lookA = p.lookA;
    switch (p.look) {
    case Look::Pen: case Look::PenRound: case Look::Hard1:
    case Look::Dash: case Look::PixelPencil: case Look::Rainbow:
    case Look::Chameleon: case Look::RibbonTwist: {
        const double soft = usesEdgeSoftness(p.look) ? qBound(0, p.edge, 100) / 100.0 : 0.0;
        return half * (1.0 + soft * 0.5) + 1.0;
    }
    case Look::Stitch:
        // Each stitch is drawn on along the path, so the last one can overshoot the end.
        return half + w * 1.1 + 3.0;
    case Look::Soft: {
        const double soft = qBound(0, p.edge, 100) / 100.0;
        return half * (1.0 + soft * 0.5) + 1.0;
    }
    case Look::Air: {
        // Factory Air is edge 90: keep that radius identical (half*1.2). Extra softness only above 90.
        const double soft = qBound(0, p.edge, 100) / 100.0;
        return half * (1.2 + qMax(0.0, soft - 0.9) * 0.5) + 1.0;
    }
    case Look::Spray:
        return half * (1.0 + lookA * 0.5) + 1.5;
    case Look::Marker: {
        const double mw = (p.lookB > 0 && p.lookB < 1) ? w * p.lookB : w;
        return mw / 2.0 + 1.0;
    }
    case Look::Highlighter:
        return half + 1.0;
    case Look::Flat: case Look::WetCalligraphy: {
        // Rotated nib (length up to width) plus the Wet ink pool.
        const bool wet = p.look == Look::WetCalligraphy;
        const bool quill = p.name.contains(QLatin1String("Quill"));
        const double r = half;
        const double nibL = r * (quill ? 1.55 : 2.0);
        const double pad = wet ? qMax(1.0, r * 0.35) : 0.0;
        return nibL / 2.0 + pad + 2.0;
    }
    case Look::Grain: {
        // Thick crayon ticks sit well outside the tip radius.
        return half * 2.6 + 2.0;
    }
    case Look::Bristle: {
        const int n = qMax(2, p.tips > 1 ? p.tips : int(lookA));
        const double span = w * (1.0 + p.tipSpacing / 100.0);
        const double jitter = (p.lookB > 0 ? w * 0.03 * (1 + p.lookB) : w * 0.02) * 3;
        const double brW = qMax(0.6, w / n * 0.8);
        // Outer strand sits at span/2, plus bristle width and wobble.
        return 0.5 * span + brW * 0.5 + jitter + 2.0;
    }
    case Look::Dots: case Look::Squares: case Look::Confetti:
    case Look::FluidSplatter: case Look::PaintSplatter: {
        const double spread = w * (lookA > 0 ? lookA : 0.8);
        const bool fluid = p.look == Look::FluidSplatter;
        const double s = fluid ? w / 3.0 * 1.7 : w / 4.0 * 1.4;
        // Fluid drips can land 6.2 * s below a scattered blob.
        return spread + (fluid ? s * 6.2 : s) + 2.0;
    }
    case Look::ToxicWaste:
        return w * 0.7 + w * 0.73 * 3.0 + 2.0;
    case Look::Squiggle:
        return half + w / 12.0 + 2.0;
    case Look::Fur:
        return w + w / 10.0 + 1.0;
    case Look::Water: case Look::WaterRandom: {
        const bool random = p.look == Look::WaterRandom;
        const double r = half * (random ? 1.05 : 1.15);
        return (random ? r * 2.9 : r * 2.2) + 2.0;
    }
    case Look::Outline: case Look::DoubleDash:
        return half + w / 4.0 + 2.0;
    case Look::ThreeD:
        return half + w * 0.24 + 2.0;
    case Look::ThreeDGlow:
        return w * 1.45 + 4.0;
    case Look::Toothpaste:
        return w * 1.15 / 2.0 + w * 0.22 + w * 0.21 + w * 0.09 + 2.0;
    case Look::AlienPaint:
        // Beads reach w*0.75; pad for AA so incremental dirty/clip covers them.
        return w * 0.75 + 4.0;
    case Look::NeonTube:
        return w * 1.2 + 2.0;
    case Look::Gouache:
        return half * 1.04 * 2.0 + 3.0;
    case Look::InkWash:
        return half * 1.3 * 2.1 + 2.0;
    case Look::Chain:
        return w * 0.62 + w * 0.09 + 2.0;
    case Look::Rope:
        return w * 0.38 + w * 0.25 + 2.0;
    case Look::Glitter:
        return w * 0.7 + w * 0.26 + 2.0;
    case Look::Smoke:
        return w * 0.9 + w * 0.55 + 2.0;
    case Look::Fire:
        return w * 3.6 + 4.0;
    case Look::Lightning:
        return w * 4.0 + 8.0;
    case Look::SprayCan: {
        if (p.sprayLook >= 1) {
            const double spread = 0.75 + 1.7 * (qBound(0, p.spraySpread, 100) / 100.0);
            // Droplets can land at 1.35 * spreadR, and spreadR itself grows with
            // pressure and slows; pad generously so incremental dirty covers them.
            double r = w * spread * 2.6 + 8.0;
            if (p.sprayDrips) r = qMax(r, w * 6.0 + 8.0);
            return r;
        }
        return w * 4.0 + 4.0;
    }
    case Look::ImageStamp:
        return w * 1.6 + 2.0;
    case Look::Cellular:
        return half + qMax(4, p.cellSize) + 2.0;
    default:
        return half + 1.0;
    }
}

double extent(const Params &p, double width)
{
    return qMax(0.0, paintedRadius(p, width) - qMax(0.5, width) / 2.0);
}

QRectF paintedBounds(const QPainterPath &path, const Params &p, double width)
{
    const double r = paintedRadius(p, width);
    QRectF b = path.boundingRect().adjusted(-r, -r, r, r);
    if (p.look == Look::Smoke) {
        // Smoke drifts upward in proportion to distance along the path.
        const double len = path.length();
        b.adjust(0, -0.16 * len, 0, 0);
    }
    return b;
}

QString paramFingerprint(const Params &p)
{
    Params q = p;
    q.key.clear();
    q.category.clear();
    q.name.clear();
    QString s;
    s += QString::number(int(q.look)) + QChar(',') + QString::number(q.lookA, 'g', 6) + QChar(',') + QString::number(q.lookB, 'g', 6);
    s += QChar('|') + QString::number(q.inkAmount) + QChar(',') + QString::number(q.spacing) + QChar(',') + QString::number(q.flowRate);
    s += QChar('|') + QString::number(q.aspect, 'g', 6) + QChar(',') + QString::number(q.angle, 'g', 6) + QChar(',') + QString::number(q.edge);
    s += QChar('|') + q.texture + QChar(',') + QString::number(q.textureAmount);
    s += QChar('|') + QString::number(q.tips) + QChar(',') + QString::number(int(q.colorVar));
    s += QChar('|') + QString::number(q.dashOn[0]) + QChar(',') + QString::number(q.dashOff[0]);
    s += QChar(',') + QString::number(q.dashOn[1]) + QChar(',') + QString::number(q.dashOff[1]);
    s += QChar('|') + QString::number(q.buildUp) + QChar(',') + QString::number(q.square) + QChar(',') + QString::number(q.size);
    s += QChar('|') + q.stampSource;
    s += QChar('|') + q.cellRule + QChar(',') + QString::number(q.cellSize) + QChar(',')
        + QString::number(q.cellSteps) + QChar(',') + QString::number(q.seedDensity)
        + QChar(',') + (q.cellAgeColor.isValid() ? q.cellAgeColor.name(QColor::HexArgb) : QString());
    s += QChar('|') + QString::number(int(q.cellSampleUnder)) + QChar(',') + QString::number(int(q.cellFollowEdges));
    s += QChar('|') + QString::number(q.sizeSens.pressure) + QChar(',') + QString::number(q.inkSens.pressure) + QChar(',') + QString::number(q.hueSens.random);
    return s;
}

QJsonObject toJson(const Params &p)
{
    const Params def = paramsFor(p.key);
    QJsonObject j;
    j.insert(QStringLiteral("key"), p.key);
    auto putI = [&](const char *n, int v, int d) { if (v != d) j.insert(QLatin1String(n), v); };
    auto putD = [&](const char *n, double v, double d) { if (!qFuzzyCompare(v + 1, d + 1)) j.insert(QLatin1String(n), v); };
    auto putB = [&](const char *n, bool v, bool d) { if (v != d) j.insert(QLatin1String(n), v); };
    putI("inkAmount", p.inkAmount, def.inkAmount);
    putI("spacing", p.spacing, def.spacing);
    putI("flowRate", p.flowRate, def.flowRate);
    putB("buildUp", p.buildUp, def.buildUp);
    if (p.texture != def.texture) j.insert(QStringLiteral("texture"), p.texture);
    putI("textureAmount", p.textureAmount, def.textureAmount);
    putI("edgeTexture", p.edgeTexture, def.edgeTexture);
    if (p.edgeEffect != def.edgeEffect) j.insert(QStringLiteral("edgeEffect"), p.edgeEffect);
    putI("tips", p.tips, def.tips);
    putI("tipSpacing", p.tipSpacing, def.tipSpacing);
    if (p.colorVar != def.colorVar) j.insert(QStringLiteral("colorVar"), int(p.colorVar));
    if (p.dashOn[0] != def.dashOn[0] || p.dashOff[0] != def.dashOff[0]
        || p.dashOn[1] != def.dashOn[1] || p.dashOff[1] != def.dashOff[1]
        || p.dashOn[2] != def.dashOn[2] || p.dashOff[2] != def.dashOff[2]) {
        QJsonArray on, off;
        for (int i = 0; i < 3; ++i) { on.append(p.dashOn[i]); off.append(p.dashOff[i]); }
        j.insert(QStringLiteral("dashOn"), on);
        j.insert(QStringLiteral("dashOff"), off);
    }
    putB("square", p.square, def.square);
    putD("size", p.size, def.size);
    putI("edge", p.edge, def.edge);
    putD("aspect", p.aspect, def.aspect);
    putD("angle", p.angle, def.angle);
    auto putSens = [&](const char *n, const Sensitivity &s, const Sensitivity &d) {
        if (s == d) return;
        QJsonObject o{{"pressure", s.pressure}, {"speed", s.speed},
                      {"horizontal", s.horizontal}, {"vertical", s.vertical}, {"random", s.random}};
        j.insert(QLatin1String(n), o);
    };
    putSens("sizeSens", p.sizeSens, def.sizeSens);
    putSens("inkSens", p.inkSens, def.inkSens);
    putSens("satSens", p.satSens, def.satSens);
    putSens("hueSens", p.hueSens, def.hueSens);
    putSens("angleSens", p.angleSens, def.angleSens);
    if (p.stampSource != def.stampSource) j.insert(QStringLiteral("stampSource"), p.stampSource);
    if (p.cellRule != def.cellRule) j.insert(QStringLiteral("cellRule"), p.cellRule);
    putI("cellSize", p.cellSize, def.cellSize);
    putI("cellSteps", p.cellSteps, def.cellSteps);
    putI("seedDensity", p.seedDensity, def.seedDensity);
    if (p.cellAgeColor.isValid() && p.cellAgeColor != def.cellAgeColor)
        j.insert(QStringLiteral("cellAgeColor"), p.cellAgeColor.name(QColor::HexArgb));
    if (p.cellSampleUnder) j.insert(QStringLiteral("cellSampleUnder"), true);
    if (p.cellFollowEdges) j.insert(QStringLiteral("cellFollowEdges"), true);
    // Always record sprayLook on Spray Can so a missing key still means the legacy look.
    if (p.look == Look::SprayCan) j.insert(QStringLiteral("sprayLook"), p.sprayLook);
    putI("sprayDensity", p.sprayDensity, def.sprayDensity);
    putI("spraySpread", p.spraySpread, def.spraySpread);
    putB("sprayDrips", p.sprayDrips, def.sprayDrips);
    return j;
}

static Sensitivity sensFrom(const QJsonObject &o, Sensitivity s)
{
    auto gi = [&](const char *n, int d) { return qBound(0, o[QLatin1String(n)].toInt(d), 100); };
    s.pressure = gi("pressure", s.pressure);
    s.speed = gi("speed", s.speed);
    s.horizontal = gi("horizontal", s.horizontal);
    s.vertical = gi("vertical", s.vertical);
    s.random = gi("random", s.random);
    return s;
}

Params fromJson(const QJsonObject &j, const Params &base)
{
    Params p = base;
    if (p.key.isEmpty()) p = paramsFor(j[QStringLiteral("key")].toString(QStringLiteral("basic.hard")));
    else if (j.contains(QStringLiteral("key"))) p = paramsFor(j[QStringLiteral("key")].toString(p.key));
    auto gi = [&](const char *n, int d, int lo, int hi) { return j.contains(QLatin1String(n)) ? qBound(lo, j[QLatin1String(n)].toInt(d), hi) : d; };
    auto gd = [&](const char *n, double d) { return j.contains(QLatin1String(n)) ? j[QLatin1String(n)].toDouble(d) : d; };
    p.inkAmount = gi("inkAmount", p.inkAmount, 0, 100);
    p.spacing = gi("spacing", p.spacing, 0, 400);
    p.flowRate = gi("flowRate", p.flowRate, 0, 100);
    if (j.contains(QStringLiteral("buildUp"))) p.buildUp = j[QStringLiteral("buildUp")].toBool();
    if (j.contains(QStringLiteral("texture"))) p.texture = j[QStringLiteral("texture")].toString();
    p.textureAmount = gi("textureAmount", p.textureAmount, 0, 100);
    p.edgeTexture = gi("edgeTexture", p.edgeTexture, 0, 100);
    if (j.contains(QStringLiteral("edgeEffect"))) p.edgeEffect = j[QStringLiteral("edgeEffect")].toString();
    p.tips = gi("tips", p.tips, 1, 32);
    p.tipSpacing = gi("tipSpacing", p.tipSpacing, 0, 400);
    if (j.contains(QStringLiteral("colorVar"))) p.colorVar = ColorVariation(qBound(0, j[QStringLiteral("colorVar")].toInt(), 5));
    if (j.contains(QStringLiteral("dashOn")) && j[QStringLiteral("dashOn")].isArray()) {
        const QJsonArray on = j[QStringLiteral("dashOn")].toArray();
        const QJsonArray off = j[QStringLiteral("dashOff")].toArray();
        for (int i = 0; i < 3; ++i) {
            p.dashOn[i] = i < on.size() ? qMax(0, on[i].toInt()) : 0;
            p.dashOff[i] = i < off.size() ? qMax(0, off[i].toInt()) : 0;
        }
    }
    if (j.contains(QStringLiteral("square"))) p.square = j[QStringLiteral("square")].toBool();
    p.size = qBound(0.25, gd("size", p.size), 500.0);
    p.edge = gi("edge", p.edge, 0, 100);
    p.aspect = qBound(0.01, gd("aspect", p.aspect), 1.0);
    p.angle = gd("angle", p.angle);
    if (j.contains(QStringLiteral("sizeSens"))) p.sizeSens = sensFrom(j[QStringLiteral("sizeSens")].toObject(), p.sizeSens);
    if (j.contains(QStringLiteral("inkSens"))) p.inkSens = sensFrom(j[QStringLiteral("inkSens")].toObject(), p.inkSens);
    if (j.contains(QStringLiteral("satSens"))) p.satSens = sensFrom(j[QStringLiteral("satSens")].toObject(), p.satSens);
    if (j.contains(QStringLiteral("hueSens"))) p.hueSens = sensFrom(j[QStringLiteral("hueSens")].toObject(), p.hueSens);
    if (j.contains(QStringLiteral("angleSens"))) p.angleSens = sensFrom(j[QStringLiteral("angleSens")].toObject(), p.angleSens);
    if (j.contains(QStringLiteral("stampSource"))) p.stampSource = j[QStringLiteral("stampSource")].toString();
    if (j.contains(QStringLiteral("cellRule"))) p.cellRule = j[QStringLiteral("cellRule")].toString();
    p.cellSize = gi("cellSize", p.cellSize, 1, 32);
    p.cellSteps = gi("cellSteps", p.cellSteps, 0, 16);
    p.seedDensity = gi("seedDensity", p.seedDensity, 0, 100);
    if (j.contains(QStringLiteral("cellAgeColor"))) p.cellAgeColor = QColor(j[QStringLiteral("cellAgeColor")].toString());
    p.cellSampleUnder = j[QStringLiteral("cellSampleUnder")].toBool(false);
    p.cellFollowEdges = j[QStringLiteral("cellFollowEdges")].toBool(false);
    if (p.look == Look::SprayCan) {
        // Missing key: a pre-0.3.29 document, keep the legacy renderer.
        p.sprayLook = j.contains(QStringLiteral("sprayLook"))
                          ? gi("sprayLook", 0, 0, 4) : 0;
        p.sprayDensity = gi("sprayDensity", p.sprayDensity, 0, 100);
        p.spraySpread = gi("spraySpread", p.spraySpread, 0, 100);
        if (j.contains(QStringLiteral("sprayDrips")))
            p.sprayDrips = j[QStringLiteral("sprayDrips")].toBool();
    }
    return p;
}



constexpr int kStampVariants = 8;

struct StampAtlas {
    QImage img[kStampVariants];
};

struct StampKey {
    quint32 seed = 0, aux = 0, rgba = 0;
    int dim = 0, n = 0;
    bool operator==(const StampKey &o) const {
        return seed == o.seed && aux == o.aux && rgba == o.rgba && dim == o.dim && n == o.n;
    }
};

const StampAtlas &cachedStampAtlas(const StampKey &key, const std::function<void(StampAtlas &)> &bake)
{
    thread_local StampKey last;
    thread_local StampAtlas atlas;
    thread_local bool ready = false;
    if (ready && last == key) return atlas;
    StampAtlas next;
    bake(next);
    atlas = std::move(next);
    last = key;
    ready = true;
    return atlas;
}


void paintSprayAir(QPainter &p, const QPainterPath &path, const PaintInput &in,
                   const QColor &cOrig, double w)
{
    if (path.isEmpty() || w <= 0) return;
    const WalkOriginScope _walkOrigin(in.pathOrigin);
    const Params &d = in.params;
    const double dens = qBound(0, d.sprayDensity, 100) / 100.0;
    const double spreadK = 0.75 + 1.7 * (qBound(0, d.spraySpread, 100) / 100.0);
    const double flowK = qBound(0.05, (d.flowRate > 0 ? d.flowRate : 75) / 100.0, 1.5);
    const double inkK = qBound(0.05, d.inkAmount / 100.0, 1.0);
    const QRectF bounds = path.controlPointRect();

    struct Sample {
        QPointF pt;
        double dist = 0;
        double press = 1;
        double speed = 1;
        double time = -1;
    };
    QVector<Sample> samples;
    QPointF lastPt;
    double lastDist = 0;
    bool have = false;
    walk(path, spaceStep(d, qMax(0.4, w * 0.12)), [&](QPointF pt, QPointF, double dist) {
        Sample s;
        s.pt = pt;
        s.dist = dist;
        s.press = qBound(0.05, in.pressureAt(dist), 1.0);
        s.speed = qBound(0.05, in.speedAt(dist), 3.0);
        s.time = in.sampleTime.isEmpty() ? -1.0 : in.timeAt(dist);
        samples.append(s);
        lastPt = pt;
        lastDist = dist;
        have = true;
    });
    if (samples.isEmpty()) return;

    // Synthesize times when the stroke has no clock (vector path, old live path).
    if (in.sampleTime.isEmpty()) {
        samples[0].time = 0;
        const double typ = qMax(8.0, w) * 0.45; // px per 16 ms at speed 1
        for (int i = 1; i < samples.size(); ++i) {
            const double dd = qMax(0.0, samples[i].dist - samples[i - 1].dist);
            const double sp = qMax(0.15, samples[i].speed);
            samples[i].time = samples[i - 1].time + dd / (sp * typ) * 16.0;
        }
    }

    auto gauss = [](std::mt19937 &rng) {
        std::uniform_real_distribution<double> U(0, 1);
        const double u = qMax(1e-9, U(rng)), v = U(rng);
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2 * M_PI * v);
    };

    p.save();
    p.setPen(Qt::NoPen);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const int i0 = (!samples.isEmpty() && in.pathOrigin > 0.5
                    && std::abs(samples[0].dist - in.pathOrigin) < 0.6
                    && samples.size() > 1) ? 1 : 0;
    for (int i = i0; i < samples.size(); ++i) {
        const Sample &s = samples[i];
        const QColor col = senseColor(cOrig, d, in, s.pt, s.dist, bounds);
        const double sk = senseMul(d.sizeSens, in, s.pt, s.dist, bounds);
        const double press = s.press;
        const double speed = s.speed;
        // Mid-path suffixes used to treat samples[0] as a stroke start (dt=16),
        // which rebuilt a much denser spray than a same-sample full paint.
        const bool strokeStart = (i == 0 && s.dist < 0.5);
        const double prevTime = strokeStart ? (s.time - 16.0)
            : (i > 0) ? samples[i - 1].time
                      : in.timeAt(qMax(0.0, s.dist - qMax(0.4, w * 0.12)));
        const double dtCap = strokeStart ? 16.0
            : ((i > 0 && QLineF(samples[i - 1].pt, s.pt).length() < 0.6) ? 240.0 : 80.0);
        const double dt = qBound(0.0, s.time - prevTime, dtCap);
        // Pressure raises flow and spread; speed thins and sparsifies.
        const double flow = flowK * inkK * (0.25 + 0.75 * press) * (0.35 + 0.65 / qMax(0.2, speed));
        const double spreadR = qMax(1.2, w * spreadK * sk * (0.55 + 0.55 * press) * (0.55 + 0.45 / qMax(0.25, speed)));
        const double coreR = qMax(0.8, spreadR * (0.32 + 0.1 * press) * (0.65 + 0.2 / qMax(0.25, speed)));

        // Soft Gaussian core — scales with dwell (dt) and flow so holding still builds up.
        // Without Build-up, dwell does not pile on and overlap stays thinner.
        const bool build = d.buildUp;
        const double dwellK = build ? qBound(0.15, dt / 18.0, 3.5) : 1.0;
        const double coreA = qBound(0.0, col.alphaF() * flow * dwellK * (build ? 0.22 : 0.12),
                                    build ? 0.55 : 0.22);
        if (coreA > 0.004 && coreR > 0.4) {
            QColor inC = col;
            inC.setAlphaF(float(coreA));
            QColor mid = col;
            mid.setAlphaF(float(coreA * 0.45));
            QColor out = col;
            out.setAlpha(0);
            QRadialGradient g(s.pt, coreR);
            g.setColorAt(0.0, inC);
            g.setColorAt(0.45, mid);
            g.setColorAt(1.0, out);
            p.setBrush(g);
            p.drawEllipse(s.pt, coreR, coreR);
        }

        const double budget = (6.0 + 36.0 * dens) * flow
                              * (build ? qBound(0.2, dt / 16.0, 4.0) : 1.0)
                              * (build ? 1.0 : 0.65);
        int n = int(std::floor(budget));
        const quint32 distKey = quint32(std::llround(s.dist * 1024.0));
        std::mt19937 extraRng(in.seed ^ (distKey * 2246822519u) ^ 0x9e3779b9u);
        std::uniform_real_distribution<double> EU(0, 1);
        if (EU(extraRng) < (budget - n)) ++n;
        n = qBound(0, n, 80);

        for (int k = 0; k < n; ++k) {
            std::mt19937 rng(in.seed ^ (distKey * 2654435761u) ^ (quint32(k + 1) * 2246822519u));
            std::uniform_real_distribution<double> U(0, 1);
            const double roll = U(rng);
            const bool spatter = roll > 0.93;
            const bool mist = !spatter && roll > 0.62;
            const double sigma = spreadR * (spatter ? 0.85 : mist ? 0.72 : 0.32);
            QPointF off(gauss(rng) * sigma, gauss(rng) * sigma);
            const double rad2 = off.x() * off.x() + off.y() * off.y();
            const double maxR = spreadR * 1.35;
            if (rad2 > maxR * maxR) off *= maxR / std::sqrt(rad2);
            const QPointF pt = s.pt + off;
            const double drop = spatter
                                    ? (1.4 + 2.6 * U(rng)) * (0.7 + 0.5 * press)
                                    : mist ? (0.35 + 0.9 * U(rng))
                                           : (0.45 + 1.0 * U(rng));
            QColor dc = col;
            const double a = col.alphaF() * inkK * (spatter ? 0.85 : mist ? 0.28 : 0.5)
                             * (0.5 + 0.5 * press) * (0.7 + 0.3 * U(rng));
            dc.setAlphaF(float(qBound(0.0, a, 0.95)));
            p.setBrush(dc);
            p.drawEllipse(pt, drop, drop * (spatter ? 0.85 + 0.2 * U(rng) : 1.0));
        }

        if (d.sprayDrips && press > 0.12) {
            std::mt19937 rng(in.seed ^ (distKey * 1597334677u) ^ 0x85ebca6bu);
            std::uniform_real_distribution<double> U(0, 1);
            const double weight = press * (0.35 + 0.65 / qMax(0.2, speed))
                                  * qBound(0.4, dt / 16.0, 2.5);
            // Guaranteed drips on a regular cadence so the option is always visible,
            // plus a speed/pressure-weighted chance for extra ones.
            const int cadence = int(std::llround(s.dist / qMax(1.0, w * 0.12)));
            const double chance = ((cadence % 6) == 0) ? 1.0 : qBound(0.04, 0.18 * weight, 0.55);
            if (U(rng) < chance) {
                const double len = w * (0.7 + 2.6 * U(rng)) * (0.4 + 0.6 * press)
                                   * (0.45 + 0.8 / qMax(0.25, speed));
                QColor drip = col;
                drip.setAlphaF(float(qBound(0.0, col.alphaF() * inkK * 0.85, 1.0)));
                p.setPen(qpen(drip, qMax(0.7, w * 0.14 * sk), Qt::RoundCap, Qt::RoundJoin));
                const QPointF end = s.pt + QPointF((U(rng) - 0.5) * w * 0.22, len);
                p.drawLine(s.pt, end);
                p.setPen(Qt::NoPen);
                p.setBrush(drip);
                p.drawEllipse(end, w * 0.14 * sk, w * 0.18 * sk);
            }
        }
    }
    p.restore();
    Q_UNUSED(have);
    Q_UNUSED(lastPt);
    Q_UNUSED(lastDist);
}

void paint(QPainter &p, const QPainterPath &path0, const PaintInput &in)
{
    if (in.width <= 0 || path0.isEmpty()) return;
    const Params &d = in.params;
    QPainterPath path = path0;
    double w = in.width;
    p.save();
    if (in.closedShape && in.align != 0) {
        w *= 2;
        if (in.align == 1) p.setClipPath(path, Qt::IntersectClip);
        else {
            QPainterPath outer;
            outer.setFillRule(Qt::OddEvenFill);
            const double m = w * 4 + extent(d, in.width) * 2 + 10;
            outer.addRect(path.boundingRect().adjusted(-m, -m, m, m));
            outer.addPath(path);
            p.setClipPath(outer, Qt::IntersectClip);
        }
    }
    QColor c = in.color;
    // Looks that already multiply ink (absolute ink/100 or factory-relative
    // inkScale) in their branch must not get a second factor on the source
    // color. Other looks scale here; their factory ink is typically 100, so
    // a user cut is visible, and a boost cannot go past opaque.
    switch (d.look) {
    case Look::Pen: case Look::Marker: case Look::Highlighter:
    case Look::Soft: case Look::Air:
    case Look::Flat: case Look::WetCalligraphy:
    case Look::Water: case Look::WaterRandom:
    case Look::InkWash: case Look::SprayCan: case Look::ImageStamp:
    case Look::Smoke:
        break;
    default: {
        const double inkK = inkScale(d);
        if (inkK != 1.0)
            c.setAlphaF(float(qBound(0.0, c.alphaF() * inkK, 1.0)));
        break;
    }
    }
    const WalkOriginScope _walkOrigin(in.pathOrigin);
    std::mt19937 rng(in.seed);
    std::uniform_real_distribution<double> U(0, 1);
    const bool sharp = in.sharpRect;
    p.setPen(Qt::NoPen);
    const Look look = d.look;
    const QColor cOrig = c;
    const QRectF sBounds = path.controlPointRect();
    auto sKsz = [&](QPointF pt, double dist) { return senseMul(d.sizeSens, in, pt, dist, sBounds); };
    auto sCol = [&](QColor base, QPointF pt, double dist) { return senseColor(base, d, in, pt, dist, sBounds); };
    auto sAng = [&](double deg, QPointF pt, double dist) { return senseAngle(deg, d, in, pt, dist, sBounds); };
    auto varyStroke = [&](const QPainterPath &pp, const QColor &col, double ww,
                          Qt::PenCapStyle cap, Qt::PenJoinStyle join) {
        strokeMaybeVary(p, pp, in, d, col, ww, cap, join, sBounds);
    };
    switch (look) {
    case Look::Pen: {
        if (d.lookB > 0) { w = 1; p.setRenderHint(QPainter::Antialiasing, true); }
        if (d.edge > 0 && usesEdgeSoftness(Look::Pen) && d.lookB <= 0) {
            const double soft = qBound(0, d.edge, 100) / 100.0;
            const double r = qMax(0.5, w / 2) * (1.0 + soft * 0.5);
            const double hard = 1.0 - soft;
            QColor pc = c;
            const double facInk = (d.inkAmount / 100.0);
            if (facInk != 1.0)
                pc.setAlphaF(float(qBound(0.0, c.alphaF() * facInk, 1.0)));
            p.setPen(Qt::NoPen);
            walk(path, spaceStep(d, qMax(0.5, r * 0.25)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
                QColor gIn = c, out = c;
                const double facInk = (d.inkAmount / 100.0);
                if (facInk != 1.0)
                    gIn.setAlphaF(float(qBound(0.0, c.alphaF() * facInk, 1.0)));
                out.setAlpha(0);
                QRadialGradient g(pt, r);
                g.setColorAt(0, gIn);
                g.setColorAt(qBound(0.0, hard, 0.85), gIn);
                g.setColorAt(1, out);
                p.setBrush(g);
                p.drawEllipse(pt, r, r);
            });
            break;
        }
        QColor pc = c;
        const double facInk = (d.inkAmount / 100.0);
        if (facInk != 1.0)
            pc.setAlphaF(float(qBound(0.0, c.alphaF() * facInk, 1.0)));
        varyStroke(path, pc, w, sharp ? Qt::SquareCap : Qt::RoundCap, sharp ? Qt::MiterJoin : Qt::RoundJoin);
        break;
    }
    case Look::PenRound:
        if (d.edge > 0) {
            const double soft = qBound(0, d.edge, 100) / 100.0;
            const double r = qMax(0.5, w / 2) * (1.0 + soft * 0.5);
            const double hard = 1.0 - soft;
            p.setPen(Qt::NoPen);
            walk(path, spaceStep(d, qMax(0.5, r * 0.25)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
                QColor gIn = c, out = c;
                out.setAlpha(0);
                QRadialGradient g(pt, r);
                g.setColorAt(0, gIn);
                g.setColorAt(qBound(0.0, hard, 0.85), gIn);
                g.setColorAt(1, out);
                p.setBrush(g);
                p.drawEllipse(pt, r, r);
            });
            break;
        }
        varyStroke(path, c, w, Qt::RoundCap, Qt::RoundJoin);
        break;
    case Look::Hard1:
        p.setRenderHint(QPainter::Antialiasing, false);
        varyStroke(path, c, 1, Qt::SquareCap, Qt::MiterJoin);
        break;
    case Look::Marker: {
        // Felt tip: rounded rectangular stamps at mid-alpha so overlaps darken (Fireworks build-up).
        const double mw = d.lookB > 0 && d.lookB < 1 ? w * d.lookB : w;
        const double r = qMax(0.6, mw / 2);
        QColor mc = c;
        mc.setAlphaF(float(qBound(0.15, c.alphaF() * d.lookA * (d.inkAmount / 100.0) * 0.55, 0.85)));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        walk(path, spaceStep(d, qMax(0.4, r * 0.18)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            QColor mc = c;
            mc.setAlphaF(float(qBound(0.15, c.alphaF() * d.lookA * (d.inkAmount / 100.0) * 0.55, 0.85)));
            p.setBrush(mc);
            p.save();
            p.translate(pt);
            p.rotate(std::atan2(dir.y(), dir.x()) * 180 / M_PI);
            p.drawEllipse(QPointF(0, 0), r * 0.85, r);
            p.restore();
        });
        break;
    }
    case Look::Highlighter: {
        QColor mc = c;
        mc.setAlphaF(float(qBound(0.12, c.alphaF() * d.lookA * 0.7 * inkScale(d), 0.4)));
        p.setCompositionMode(QPainter::CompositionMode_Multiply);
        p.setPen(Qt::NoPen);
        const double r = qMax(0.6, w / 2);
        walk(path, spaceStep(d, qMax(0.4, r * 0.16)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            QColor mc = c;
            mc.setAlphaF(float(qBound(0.12, c.alphaF() * d.lookA * 0.7 * inkScale(d), 0.4)));
            p.setBrush(mc);
            p.save();
            p.translate(pt);
            p.rotate(std::atan2(dir.y(), dir.x()) * 180 / M_PI);
            p.drawEllipse(QPointF(0, 0), r * 0.7, r);
            p.restore();
        });
        break;
    }
    case Look::Dash: {
        QPen pn = qpen(c, qMax(1.0, w), Qt::FlatCap, sharp ? Qt::MiterJoin : Qt::RoundJoin);
        const QVector<qreal> pat = dashPattern(d);
        if (!pat.isEmpty()) pn.setDashPattern(pat);
        else pn.setDashPattern({d.lookA, d.lookB});
        const bool ramp = senseVaries(d.sizeSens, in) || senseVaries(d.inkSens, in)
                          || senseVaries(d.satSens, in) || senseVaries(d.hueSens, in);
        if (!ramp) {
            const QPointF mid = path.pointAtPercent(0.5);
            pn.setColor(sCol(cOrig, mid, 0));
            pn.setWidthF(qMax(0.2, pn.widthF() * sKsz(mid, 0)));
            const double ww0 = qMax(0.25, pn.widthF());
            pn.setDashOffset(in.pathOrigin / ww0);
            p.strokePath(path, pn);
        } else {
            QPointF prev;
            bool has = false;
            const QColor base = pn.color();
            const double ww = pn.widthF();
            walk(path, qMax(0.4, ww * 0.2), [&](QPointF pt, QPointF, double dist) {
                if (has) {
                    QPen q = pn;
                    q.setColor(sCol(base, pt, dist));
                    q.setWidthF(qMax(0.2, ww * sKsz(pt, dist)));
                    q.setDashOffset(dist / qMax(0.25, ww));
                    p.setPen(q);
                    p.setBrush(Qt::NoBrush);
                    p.drawLine(prev, pt);
                }
                prev = pt;
                has = true;
            });
        }
        break;
    }
    case Look::DoubleDash: case Look::Outline: {
        QPainterPathStroker st;
        st.setWidth(qMax(2.0, w));
        st.setJoinStyle(Qt::RoundJoin);
        st.setCapStyle(Qt::RoundCap);
        QPainterPath outline = st.createStroke(path).simplified();
        QPen pn = qpen(c, qMax(1.0, w / 4), Qt::FlatCap, Qt::RoundJoin);
        if (look == Look::DoubleDash) {
            const QVector<qreal> pat = dashPattern(d);
            if (!pat.isEmpty()) pn.setDashPattern(pat);
            else pn.setDashPattern({d.lookA, d.lookB});
        }
        const bool ramp = senseVaries(d.sizeSens, in) || senseVaries(d.inkSens, in)
                          || senseVaries(d.satSens, in) || senseVaries(d.hueSens, in);
        if (!ramp) {
            const QPointF mid = path.pointAtPercent(0.5);
            pn.setColor(sCol(cOrig, mid, 0));
            pn.setWidthF(qMax(0.2, pn.widthF() * sKsz(mid, 0)));
            if (look == Look::DoubleDash) {
                const double ww0 = qMax(0.25, pn.widthF());
                pn.setDashOffset(in.pathOrigin / ww0);
            }
            p.strokePath(outline, pn);
        } else {
            QPointF prev;
            bool has = false;
            const QColor base = pn.color();
            const double ww = pn.widthF();
            walk(outline, qMax(0.4, ww * 0.2), [&](QPointF pt, QPointF, double dist) {
                if (has) {
                    QPen q = pn;
                    q.setColor(sCol(base, pt, dist));
                    q.setWidthF(qMax(0.2, ww * sKsz(pt, dist)));
                    if (look == Look::DoubleDash) q.setDashOffset(dist / qMax(0.25, ww));
                    p.setPen(q);
                    p.setBrush(Qt::NoBrush);
                    p.drawLine(prev, pt);
                }
                prev = pt;
                has = true;
            });
        }
        break;
    }
    case Look::ThreeD: case Look::ThreeDGlow: {
        if (look == Look::ThreeDGlow) {
            // Smooth coloured halo from the stroke colour. Overlapping soft stamps,
            // SourceOver only — stacked opaque rings + Plus read as grey bands on a
            // light background.
            QColor glow = c;
            int hh, ss, vv, aa;
            glow.getHsv(&hh, &ss, &vv, &aa);
            if (ss > 20) {
                ss = qMin(255, ss + 30);
                vv = qBound(70, vv + 50, 230);
            } else {
                // near-neutral: keep the hue of the ink, lift just enough to show a soft aura
                vv = qBound(40, vv + 35, 160);
            }
            glow.setHsv(hh < 0 ? 0 : hh, ss, vv, aa);
            const double R = w * 1.45 + 2;
            p.save();
            p.setPen(Qt::NoPen);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            walk(path, spaceStep(d, qMax(0.5, w * 0.16)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
                QColor gIn = glow;
                gIn.setAlphaF(qBound(0.0, c.alphaF() * 0.22, 0.4));
                QColor mid = glow;
                mid.setAlphaF(float(qBound(0.0, c.alphaF() * 0.10, 0.22)));
                QColor out = glow;
                out.setAlpha(0);
                QRadialGradient g(pt, R);
                g.setColorAt(0.0, gIn);
                g.setColorAt(0.4, mid);
                g.setColorAt(1.0, out);
                p.setBrush(g);
                p.drawEllipse(pt, R, R);
            });
            p.restore();
        }
        // Tube: deep shadow, body, sharp top-left highlight
        varyStroke(path, c.darker(190), w, Qt::RoundCap, Qt::RoundJoin);
        varyStroke(path, c.darker(120), w * 0.82, Qt::RoundCap, Qt::RoundJoin);
        varyStroke(path, c, w * 0.58, Qt::RoundCap, Qt::RoundJoin);
        p.save();
        p.translate(-w * 0.18, -w * 0.2);
        varyStroke(path, c.lighter(160), qMax(1.0, w * 0.28), Qt::RoundCap, Qt::RoundJoin);
        p.translate(-w * 0.06, -w * 0.06);
        QColor hi = QColor(255, 255, 255, int(c.alpha() * 0.7));
        varyStroke(path, hi, qMax(0.8, w * 0.1), Qt::RoundCap, Qt::RoundJoin);
        p.restore();
        break;
    }
    case Look::Soft: case Look::Air: {
        const double soft = qBound(0, d.edge, 100) / 100.0;
        const double r = qMax(0.5, w / 2) * (look == Look::Soft
            ? (1.0 + soft * 0.5)
            : (1.2 + qMax(0.0, soft - 0.9) * 0.5));
        // Air already stores coverage in inkAmount (from lookA). Don't multiply lookA again
        // or Basic Air Brush is almost invisible (~lookA²).
        const double alpha = look == Look::Soft ? 0.5 : 1.0;
        // Factory Air (edge 90) stays fully soft (hard 0), matching the old lookA path.
        const double hard = look == Look::Soft ? (1.0 - soft)
                                               : qBound(0.0, 1.0 - soft / 0.9, 1.0);
        walk(path, spaceStep(d, qMax(0.5, r * (look == Look::Soft ? 0.25 : 0.35))), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            QColor gIn = c, out = c;
            gIn.setAlphaF(qBound(0.0, c.alphaF() * alpha * (d.inkAmount / 100.0), 1.0));
            out.setAlpha(0);
            QRadialGradient g(pt, r);
            g.setColorAt(0, gIn);
            g.setColorAt(qBound(0.0, hard, 0.85), gIn);
            g.setColorAt(1, out);
            p.setBrush(g);
            p.drawEllipse(pt, r, r);
        });
        break;
    }
    case Look::Water: case Look::WaterRandom: {
        // Continuous translucent wash + darker pooling along the wet edge (not a string of rings).
        const bool random = look == Look::WaterRandom;
        const double r = qMax(0.8, w / 2) * (random ? 1.05 : 1.15);
        const double alpha = qBound(0.18, d.lookA * 0.85 + 0.14, 0.45);
        QPainterPathStroker st;
        st.setWidth(qMax(1.0, w * (random ? 0.9 : 1.15)));
        st.setCapStyle(Qt::RoundCap);
        st.setJoinStyle(Qt::RoundJoin);
        const QPainterPath cover = st.createStroke(path);
        const bool washRamp = senseVaries(d.sizeSens, in) || senseVaries(d.inkSens, in)
                              || senseVaries(d.satSens, in) || senseVaries(d.hueSens, in);
        auto washAt = [&](QPointF pt, double dist) {
            QColor wash = sCol(cOrig, pt, dist);
            wash.setAlphaF(float(qBound(0.0, wash.alphaF() * alpha * inkScale(d), 0.5)));
            return wash;
        };
        if (!washRamp) {
            QColor wash = washAt(path.pointAtPercent(0.5), 0);
            p.setPen(Qt::NoPen);
            p.setBrush(wash);
            p.drawPath(cover);
            QColor edge = wash.darker(150);
            edge.setAlphaF(float(qBound(0.0, wash.alphaF() * 1.6, 0.55)));
            QPainterPathStroker et;
            et.setWidth(qMax(1.0, w * 0.18));
            et.setCapStyle(Qt::RoundCap);
            et.setJoinStyle(Qt::RoundJoin);
            QPainterPath rim = et.createStroke(cover);
            p.setBrush(edge);
            p.drawPath(rim);
        } else {
            QPointF prev;
            bool has = false;
            const double wCover = qMax(1.0, w * (random ? 0.9 : 1.15));
            const double wRim = qMax(1.0, w * 0.18);
            walk(path, qMax(0.5, w * 0.2), [&](QPointF pt, QPointF, double dist) {
                if (has) {
                    const QColor wash = washAt(pt, dist);
                    const double sk = sKsz(pt, dist);
                    p.setPen(qpen(wash, qMax(0.6, wCover * sk), Qt::RoundCap, Qt::RoundJoin));
                    p.setBrush(Qt::NoBrush);
                    p.drawLine(prev, pt);
                    QColor edge = wash.darker(150);
                    edge.setAlphaF(float(qBound(0.0, wash.alphaF() * 1.6, 0.55)));
                    p.setPen(qpen(edge, qMax(0.5, wRim * sk), Qt::RoundCap, Qt::RoundJoin));
                    p.drawLine(prev, pt);
                }
                prev = pt;
                has = true;
            });
        }
        // occasional puddles / random blooms
        walk(path, spaceStep(d, qMax(1.0, r * (random ? 0.7 : 1.1))), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 rng = rngAt(in.seed, dist);
            if (!random && U(rng) < 0.55) return;
            QPointF q = pt;
            if (random) {
                const double a = U(rng) * 2 * M_PI, rr = r * (0.3 + 1.2 * U(rng));
                q += QPointF(std::cos(a), std::sin(a)) * rr;
            } else {
                const QPointF n = QPointF(0, 1); // pooling slightly off the centre
                q += QPointF((U(rng) - 0.5) * r, (U(rng) - 0.5) * r * 0.6);
                Q_UNUSED(n);
            }
            const double rad = r * (random ? (0.5 + 0.9 * U(rng)) : (0.45 + 0.35 * U(rng)));
            QColor puddle = c.darker(random ? 140 : 160);
            puddle.setAlphaF(float(qBound(0.0, washAt(pt, dist).alphaF() * (0.7 + 0.5 * U(rng)), 0.5)));
            QColor z = puddle; z.setAlpha(0);
            QRadialGradient g(q, rad);
            g.setColorAt(0, puddle);
            g.setColorAt(0.6, puddle);
            g.setColorAt(1, z);
            p.setBrush(g);
            p.drawEllipse(q, rad, rad * 0.9);
            Q_UNUSED(dist);
        });
        break;
    }
    case Look::Spray: {
        const double r = qMax(1.0, w / 2) * (1 + d.lookA * 0.5);
        walk(path, spaceStep(d, qMax(1.0, r * 0.5)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            p.setBrush(c);
            std::mt19937 rng = rngAt(in.seed, dist);
            const int n = qMax(2, int(r * 1.2));
            for (int i = 0; i < n; ++i) {
                const double a = U(rng) * 2 * M_PI, rr = r * std::sqrt(U(rng));
                const double dot = 0.4 + U(rng) * 0.6;
                p.drawEllipse(pt + QPointF(std::cos(a), std::sin(a)) * rr, dot, dot);
            }
        });
        break;
    }
    case Look::Flat: case Look::WetCalligraphy: {
        // Calligraphy tips: Basic = plain fixed-angle chisel; Quill = thin hairline
        // with a sharp, tapered nib; Ribbon = a wide flat strip that twists and folds;
        // Wet = the chisel in ink, with a smooth pool and a darker rim (no blotches).
        const bool wet = look == Look::WetCalligraphy;
        const bool bamboo = d.name.contains(QLatin1String("Bamboo"));
        const bool quill = d.name.contains(QLatin1String("Quill"));
        const bool ribbon = d.name.contains(QLatin1String("Ribbon"));
        const double r = qMax(0.5, w / 2);
        QColor nc = c;
        const double ink = (wet ? 0.9 : 1.0) * (d.inkAmount / 100.0);
        nc.setAlphaF(float(qBound(0.0, c.alphaF() * ink, 1.0)));

        if (ribbon) {
            const double aspectK = qMax(0.03, d.aspect > 0 ? d.aspect : 1.0);
            QVector<QPointF> left, right;
            QVector<double> fold;
            QVector<QColor> cols;
            walk(path, spaceStep(d, qMax(0.5, w * 0.12)), [&](QPointF pt, QPointF dir, double dist) {
                const QColor c = sCol(cOrig, pt, dist);
                const double angRad = sAng(d.angle, pt, dist) * M_PI / 180.0;
                const double ca = std::cos(angRad), sa = std::sin(angRad);
                QPointF n(-dir.y(), dir.x());
                n = QPointF(n.x() * ca - n.y() * sa, n.x() * sa + n.y() * ca);
                const double twist = dist / qMax(10.0, w * 2.4) * M_PI + 0.35;
                const double cw = std::cos(twist);
                const double half = r * (0.18 + 0.82 * std::abs(cw)) * aspectK * sKsz(pt, dist);
                left << pt + n * half;
                right << pt - n * half;
                fold << cw;
                QColor lc = c;
                lc.setAlphaF(float(qBound(0.0, c.alphaF() * ink, 1.0)));
                cols << lc;
            });
            if (left.size() >= 2) {
                p.setPen(Qt::NoPen);
                for (int i = 0; i + 1 < left.size(); ++i) {
                    p.setBrush(cols[i]);
                    p.drawPolygon(QPolygonF({left[i], left[i + 1], right[i + 1], right[i]}));
                }
                const QColor nc = cols[cols.size() / 2];
                // slight shade on one face so the fold reads as a twist
                QPainterPath shade;
                shade.moveTo(left.first());
                for (int i = 1; i < left.size(); ++i) shade.lineTo(left[i]);
                for (int i = left.size() - 1; i >= 0; --i) {
                    const QPointF mid = (left[i] + right[i]) * 0.5;
                    shade.lineTo(mid);
                }
                shade.closeSubpath();
                QColor sh = nc.darker(125);
                sh.setAlphaF(float(qBound(0.0, nc.alphaF() * 0.35, 1.0)));
                p.setBrush(sh);
                p.drawPath(shade);
                QColor crease = nc.darker(160);
                p.setPen(qpen(crease, qMax(0.7, w * 0.06), Qt::FlatCap, Qt::RoundJoin));
                for (int i = 1; i < left.size(); ++i) {
                    if (fold[i] * fold[i - 1] <= 0 || std::abs(fold[i]) < 0.12)
                        p.drawLine(left[i], right[i]);
                }
                QColor hi = nc.lighter(170);
                hi.setAlphaF(float(qBound(0.0, nc.alphaF() * 0.7, 1.0)));
                p.setPen(qpen(hi, qMax(0.6, w * 0.07), Qt::FlatCap, Qt::RoundJoin));
                QPainterPath ridge;
                ridge.moveTo((left.first() * 0.65 + right.first() * 0.35));
                for (int i = 1; i < left.size(); ++i)
                    ridge.lineTo(left[i] * 0.65 + right[i] * 0.35);
                p.setBrush(Qt::NoBrush);
                p.drawPath(ridge);
            }
            break;
        }

        double ratio = qMax(0.03, d.aspect > 0 ? d.aspect : (d.lookA > 0 ? d.lookA : 0.2));
        double ang = d.angle;
        if (quill) {
            ratio = qMin(ratio, 0.05);
            if (qFuzzyCompare(ang, -45) || ang == 0) ang = -62;
        }
        const double nibW = qMax(0.35, r * ratio * 2);
        const double nibL = qMax(nibW + 0.5, r * (quill ? 1.55 : 2.0));
        double total = 0;
        if (quill || wet) walk(path, 2.0, [&](QPointF, QPointF, double dist) { total = dist; });
        p.setPen(Qt::NoPen);
        walk(path, spaceStep(d, qMax(0.3, nibW * (quill ? 0.28 : 0.4))), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double t = total > 1e-6 ? qBound(0.0, dist / total, 1.0) : 0.5;
            const double taper = quill ? (0.22 + 0.78 * std::sin(M_PI * t)) : 1.0;
            const double len = nibL * taper;
            const double thin = nibW * (quill ? (0.55 + 0.45 * taper) : 1.0);
            p.save();
            p.translate(pt);
            p.rotate(sAng(ang, pt, dist));
            QColor nc = c;
            nc.setAlphaF(float(qBound(0.0, c.alphaF() * ink, 1.0)));
            if (wet) {
                // smooth ink pool around the nib (no noise)
                QColor bleed = nc;
                bleed.setAlphaF(float(qBound(0.0, nc.alphaF() * 0.28, 0.5)));
                p.setBrush(bleed);
                const double pad = qMax(1.0, r * 0.35);
                p.drawRoundedRect(QRectF(-thin / 2 - pad, -len / 2 - pad * 0.6,
                                         thin + pad * 2, len + pad * 1.2),
                                  thin * 0.6 + pad, thin * 0.6 + pad);
            }
            if (bamboo) {
                const double j = 0.08 * len * (0.5 - std::fmod(std::sin(dist * 0.37 + in.seed), 1.0));
                p.setBrush(nc);
                p.drawRoundedRect(QRectF(-thin / 2, -len / 2 + j, thin, len - j * 0.4), thin * 0.15, thin * 0.15);
            } else if (quill) {
                // sharp diamond nib — pointed ends, hairline when travelling along the angle
                QPolygonF nib;
                nib << QPointF(0, -len / 2) << QPointF(thin / 2, 0)
                    << QPointF(0, len / 2) << QPointF(-thin / 2, 0);
                p.setBrush(nc);
                p.drawPolygon(nib);
            } else {
                p.setBrush(nc);
                p.drawRoundedRect(QRectF(-thin / 2, -len / 2, thin, len), thin * 0.2, thin * 0.2);
            }
            if (wet) {
                // darker, smooth rim along the long edges of the nib
                QColor rim = nc.darker(150);
                rim.setAlphaF(float(qBound(0.0, nc.alphaF() * 0.7, 1.0)));
                p.setPen(qpen(rim, qMax(0.7, thin * 0.45), Qt::FlatCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawLine(QPointF(-thin / 2, -len / 2 * 0.92), QPointF(-thin / 2, len / 2 * 0.92));
                p.drawLine(QPointF(thin / 2, -len / 2 * 0.92), QPointF(thin / 2, len / 2 * 0.92));
            }
            p.restore();
        });
        break;
    }
    case Look::Grain: {
        const double r = qMax(0.5, w / 2);
        const bool charcoal = d.category == QLatin1String("Charcoal");
        const bool crayon = d.category == QLatin1String("Crayon");
        const bool pencil = d.category == QLatin1String("Pencil") || d.lookB == 2;
        const bool textured = d.name.contains(QLatin1String("Textured"));
        const bool pastel = d.name.contains(QLatin1String("Pastel"));
        const bool creamy = d.name.contains(QLatin1String("Creamy"));
        const bool softC = charcoal && d.name.contains(QLatin1String("Soft"));
        const bool thick = d.name.contains(QLatin1String("Thick"));
        const int n = qMin(280, qMax(3, int(r * r * (thick ? 1.15 : (crayon ? 0.62 : 0.85)) * (pastel ? 0.7 : 1) + 2)));
        auto drawGrains = [&](QPainter &gp, QPointF pt, QPointF dir, std::mt19937 &gr) {
            std::uniform_real_distribution<double> GU(0, 1);
            for (int i = 0; i < n; ++i) {
                const double a = GU(gr) * 2 * M_PI;
                // broken coverage: skip some grains so the stroke is gappy
                if ((charcoal || crayon) && GU(gr) < (textured ? 0.30 : (pastel ? 0.24 : (crayon && !thick ? 0.40 : (crayon ? 0.18 : 0.10))))) continue;
                const double radj = r * (softC ? 1.25 : (thick ? 1.15 : 1.0)) * std::pow(GU(gr), crayon ? 0.35 : 0.5);
                const QPointF q = pt + QPointF(std::cos(a), std::sin(a)) * radj;
                QColor gc = c;
                if (pencil) {
                    const double sheen = 0.7 + 0.6 * GU(gr);
                    gc = c.lighter(int(100 * sheen));
                    gc.setAlphaF(float(qBound(0.0, c.alphaF() * (0.4 + 0.6 * GU(gr)), 1.0)));
                } else if (pastel) {
                    gc.setAlphaF(float(qBound(0.0, c.alphaF() * (0.2 + 0.45 * GU(gr)), 0.7)));
                } else if (creamy) {
                    gc.setAlphaF(float(qBound(0.0, c.alphaF() * (0.45 + 0.5 * GU(gr)), 1.0)));
                } else {
                    gc.setAlphaF(float(qBound(0.0, c.alphaF() * (0.35 + 0.65 * GU(gr)), 1.0)));
                }
                gp.setBrush(gc);
                if (crayon) {
                    // short waxy ticks, slightly aligned to the path (stamps are baked at dir=(1,0)
                    // and rotated onto the path when drawn)
                    const double ang = std::atan2(dir.y(), dir.x()) + (GU(gr) - 0.5) * 0.8;
                    const double len = (thick ? 1.4 : 1.0) * r * (0.25 + 0.45 * GU(gr));
                    const double tw = qMax(0.5, (thick ? 0.55 : 0.35) * r * (0.4 + GU(gr)));
                    gp.save();
                    gp.translate(q);
                    gp.rotate(ang * 180 / M_PI);
                    gp.drawRoundedRect(QRectF(-len, -tw / 2, len * 2, tw), tw * 0.4, tw * 0.4);
                    gp.restore();
                } else if (pencil) {
                    const double ang = std::atan2(dir.y(), dir.x()) + (GU(gr) - 0.5) * 0.5;
                    const double len = r * (0.15 + 0.25 * GU(gr));
                    gp.save();
                    gp.translate(q);
                    gp.rotate(ang * 180 / M_PI);
                    gp.drawEllipse(QRectF(-len, -0.35, len * 2, 0.8));
                    gp.restore();
                } else if (softC || creamy) {
                    const double s = r * (creamy ? 0.18 : 0.28) * (0.6 + GU(gr));
                    gp.drawEllipse(q, s * 1.3, s * 0.85);
                } else {
                    // charcoal flakes: irregular rounded squares
                    const double s = (textured ? 0.7 : 0.5) + GU(gr) * (textured ? 1.1 : 0.7);
                    gp.save();
                    gp.translate(q);
                    gp.rotate(GU(gr) * 180);
                    gp.drawRoundedRect(QRectF(-s, -s * 0.7, 2 * s, 1.4 * s), s * 0.25, s * 0.25);
                    gp.restore();
                }
            }
        };
        // One QPainter primitive per walk step instead of r² ellipses/rects: bake a
        // handful of grain-cluster stamps and drawImage them along the path. Same
        // grain recipe, so the character stays; a long 80 px stroke no longer takes
        // millions of draw calls (and several seconds) on every live frame.
        const int dim = qMax(8, int(std::ceil(r * 5.2 + 8)));
        const QPointF origin(dim / 2.0, dim / 2.0);
        const QPointF bakeDir(1, 0);
        StampKey key;
        key.seed = in.seed;
        key.aux = (charcoal ? 1u : 0) | (crayon ? 2u : 0) | (pencil ? 4u : 0) | (textured ? 8u : 0)
                | (pastel ? 16u : 0) | (creamy ? 32u : 0) | (softC ? 64u : 0) | (thick ? 128u : 0)
                | (quint32(qRound(r * 100)) << 8);
        key.rgba = quint32(c.rgba());
        key.dim = dim;
        key.n = n;
        const StampAtlas &atlas = cachedStampAtlas(key, [&](StampAtlas &a) {
            for (int v = 0; v < kStampVariants; ++v) {
                a.img[v] = QImage(dim, dim, QImage::Format_ARGB32_Premultiplied);
                a.img[v].fill(0);
                QPainter sp(&a.img[v]);
                sp.setRenderHint(QPainter::Antialiasing, true);
                sp.setPen(Qt::NoPen);
                std::mt19937 gr(in.seed + quint32(v + 1) * 2654435761u);
                drawGrains(sp, origin, bakeDir, gr);
            }
        });
        p.setPen(Qt::NoPen);
        walk(path, spaceStep(d, qMax(0.4, r * (crayon ? 0.42 : 0.45))), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 rng = rngAt(in.seed, dist);
            if (crayon && !thick && U(rng) < 0.16) return; // occasional wax skip along the stroke
            const int v = int(U(rng) * kStampVariants) % kStampVariants;
            const double ang = std::atan2(dir.y(), dir.x());
            p.save();
            p.translate(pt);
            p.rotate(ang * 180 / M_PI);
            drawTintedStamp(p, atlas.img[v], QPointF(-origin.x(), -origin.y()), cOrig, c);
            p.restore();
            Q_UNUSED(dist);
        });
        break;
    }
    case Look::Bristle: {
        const int n = qMax(2, d.tips > 1 ? d.tips : int(d.lookA));
        for (const QPolygonF &poly : path.toSubpathPolygons()) {
            for (int k = 0; k < n; ++k) {
                const double span = w * (1.0 + d.tipSpacing / 100.0);
                const double off = (n == 1 ? 0 : (double(k) / (n - 1) - 0.5)) * span;
                QPolygonF op = offsetPoly(poly, off, in.seed, d.lookB > 0 ? w * 0.03 * (1 + d.lookB) : w * 0.02, in.pathOrigin);
                const double brW = qMax(0.6, w / n * 0.8);
                std::mt19937 brng = rngAt(in.seed, in.pathOrigin, quint32(k + 1));
                const double brA = 0.55 + 0.45 * U(brng);
                p.setBrush(Qt::NoBrush);
                QPointF prev;
                bool has = false;
                double acc = in.pathOrigin;
                for (const QPointF &pt : op) {
                    if (has) {
                        acc += QLineF(prev, pt).length();
                        QColor bc = sCol(cOrig, pt, acc);
                        bc.setAlphaF(float(qBound(0.0, bc.alphaF() * brA, 1.0)));
                        p.setPen(qpen(bc, qMax(0.4, brW * sKsz(pt, acc)), Qt::RoundCap, Qt::RoundJoin));
                        p.drawLine(prev, pt);
                    }
                    prev = pt;
                    has = true;
                }
            }
        }
        break;
    }
    case Look::Dots: case Look::Squares: case Look::Confetti:
    case Look::FluidSplatter: case Look::PaintSplatter: {
        const double spread = w * (d.lookA > 0 ? d.lookA : 0.8);
        const bool fluid = look == Look::FluidSplatter;
        const bool isPaint = look == Look::PaintSplatter;
        walk(path, spaceStep(d, qMax(1.0, w * (fluid ? 0.85 : 0.6))), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 rng = rngAt(in.seed, dist);
            const int n = fluid ? 3 + int(U(rng) * 4) : (isPaint ? 4 + int(U(rng) * 5) : 1);
            for (int i = 0; i < n; ++i) {
                const double a = U(rng) * 2 * M_PI, rr = spread * std::sqrt(U(rng));
                double s = qMax(0.5, w / 4 * (0.4 + U(rng)));
                if (fluid) s = qMax(0.6, w / 3 * (0.3 + U(rng) * 1.4));
                QColor dc = sCol(varyColor(cOrig, d.colorVar, dist, rng), pt, dist);
                p.setBrush(dc);
                const QPointF q = pt + QPointF(std::cos(a), std::sin(a)) * rr;
                if (look == Look::Squares) p.drawRect(QRectF(q.x() - s, q.y() - s, 2 * s, 2 * s));
                else p.drawEllipse(q, s, s * (fluid && U(rng) > 0.6 ? 1.6 : 1));
                if (fluid && U(rng) > 0.75) {
                    p.drawEllipse(q + QPointF(0, s * 2 + U(rng) * s * 3), s * 0.45, s * 1.2);
                }
            }
        });
        break;
    }
    case Look::ToxicWaste: {
        // Acid slime: yellow-green glossy blobs with a luminescent rim and drips.
        walk(path, spaceStep(d, qMax(1.0, w * 0.55)), [&](QPointF pt, QPointF, double dist) {
            std::mt19937 rng = rngAt(in.seed, dist);
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const int n = 2 + int(U(rng) * 3);
            for (int i = 0; i < n; ++i) {
                const double a = U(rng) * 2 * M_PI, rr = w * 0.7 * std::sqrt(U(rng));
                const QPointF q = pt + QPointF(std::cos(a), std::sin(a)) * rr;
                const double s = qMax(0.8, w * (0.28 + 0.45 * U(rng)));
                QColor slime;
                slime.setHsl((78 + int(U(rng) * 36) + int(dist * 0.05)) % 360, 230, 140 + int(U(rng) * 50), c.alpha());
                if (c.saturation() > 80 && c.value() > 50) {
                    int h, sv, l, al; c.getHsl(&h, &sv, &l, &al);
                    slime.setHsl((h + 40 + int(U(rng) * 30)) % 360, qMax(sv, 200), qBound(80, l + 20, 220), al);
                }
                slime = senseSatHue(slime, d, in, pt, dist, sBounds);
                slime.setAlpha(c.alpha());
                QRadialGradient g(q - QPointF(s * 0.25, s * 0.3), s);
                QColor hi = slime.lighter(190); hi.setAlphaF(c.alphaF());
                QColor mid = slime; mid.setAlphaF(c.alphaF());
                QColor edge = slime.darker(150); edge.setAlphaF(c.alphaF() * 0.95);
                g.setColorAt(0, hi);
                g.setColorAt(0.4, mid);
                g.setColorAt(0.82, mid.darker(120));
                g.setColorAt(1, edge);
                p.setBrush(g);
                p.setPen(Qt::NoPen);
                p.drawEllipse(q, s, s * (0.75 + 0.4 * U(rng)));
                if (U(rng) > 0.6) {
                    p.setBrush(edge);
                    p.drawEllipse(q + QPointF((U(rng) - 0.5) * s, s * 1.3 + U(rng) * s), s * 0.25, s * 0.7);
                }
            }
        });
        break;
    }
    case Look::Squiggle: {
        const int n = qMax(1, d.tips > 1 ? d.tips : int(d.lookA));
        p.setBrush(Qt::NoBrush);
        for (int k = 0; k < n; ++k) {
            QPolygonF line;
            QVector<double> dists;
            walk(path, spaceStep(d, qMax(1.5, w * 0.6)), [&](QPointF pt, QPointF dir, double dist) {
                std::mt19937 rng = rngAt(in.seed, dist, quint32(k + 1));
                const QPointF nrm(-dir.y(), dir.x());
                line << pt + nrm * ((U(rng) - 0.5) * w * sKsz(pt, dist));
                dists << dist;
            });
            for (int i = 0; i + 1 < line.size(); ++i) {
                p.setPen(qpen(sCol(cOrig, line[i], dists[i]),
                              qMax(0.6, w / 6 * sKsz(line[i], dists[i])),
                              Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(line[i], line[i + 1]);
            }
        }
        break;
    }
    case Look::Fur: {
        walk(path, spaceStep(d, qMax(1.0, w / 8)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            p.setPen(qpen(c, qMax(0.6, w / 10), Qt::RoundCap, Qt::RoundJoin));
            std::mt19937 rng = rngAt(in.seed, dist);
            const double a = std::atan2(dir.y(), dir.x()) + (U(rng) < 0.5 ? 1 : -1) * (M_PI / 2) + (U(rng) - 0.5);
            const double len = w * (0.3 + 0.7 * U(rng));
            p.drawLine(pt, pt + QPointF(std::cos(a), std::sin(a)) * len);
        });
        break;
    }
    case Look::Chameleon: {
        walk(path, spaceStep(d, qMax(0.6, w * 0.25)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 rng = rngAt(in.seed, dist);
            QColor dc = varyColor(c, ColorVariation::Hue, dist, rng);
            p.setBrush(dc);
            p.drawEllipse(pt, w / 2, w / 2);
        });
        break;
    }
    case Look::Toothpaste: {
        // Two parallel ridges of paste on a shared body, with a dark crease between them.
        // Offset tubes alone collapse at tight bends; a centre crease stays readable.
        varyStroke(path, c.darker(170), w * 1.15, Qt::RoundCap, Qt::RoundJoin);
        varyStroke(path, c.darker(115), w * 0.98, Qt::RoundCap, Qt::RoundJoin);
        QColor c2 = c.lighter(125);
        if (c.saturation() > 20) {
            int h, s, l, a; c.getHsl(&h, &s, &l, &a);
            c2.setHsl((h + 18) % 360, s, qMin(255, l + 18), a);
        }
        QPainterPath left, right;
        const double off = w * 0.22;
        walk(path, spaceStep(d, qMax(0.7, w * 0.15)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const QPointF n(-dir.y(), dir.x());
            if (left.isEmpty()) { left.moveTo(pt + n * off); right.moveTo(pt - n * off); }
            else { left.lineTo(pt + n * off); right.lineTo(pt - n * off); }
        });
        auto ridge = [&](const QPainterPath &tp, const QColor &col) {
            varyStroke(tp, col, qMax(1.2, w * 0.42), Qt::RoundCap, Qt::RoundJoin);
            p.save();
            p.translate(-w * 0.05, -w * 0.08);
            QColor hi = col.lighter(200);
            hi.setAlphaF(float(qBound(0.0, col.alphaF() * 0.95, 1.0)));
            varyStroke(tp, hi, qMax(0.7, w * 0.14), Qt::RoundCap, Qt::RoundJoin);
            p.restore();
        };
        ridge(left.isEmpty() ? path : left, c);
        ridge(right.isEmpty() ? path : right, c2);
        QColor crease = c.darker(200);
        crease.setAlphaF(float(qBound(0.0, c.alphaF() * 0.85, 1.0)));
        varyStroke(path, crease, qMax(0.8, w * 0.10), Qt::RoundCap, Qt::RoundJoin);
        break;
    }
    case Look::AlienPaint: {
        // Viscous iridescent beads that stay connected, with a sharp wet highlight.
        varyStroke(path, c.darker(160), w * 1.25, Qt::RoundCap, Qt::RoundJoin);
        varyStroke(path, c, w * 0.95, Qt::RoundCap, Qt::RoundJoin);
        walk(path, spaceStep(d, qMax(0.7, w * 0.28)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 rng = rngAt(in.seed, dist);
            QColor dc = varyColor(c, ColorVariation::Hue, dist * 0.7, rng);
            const double rx = w * (0.55 + 0.2 * U(rng)), ry = w * (0.42 + 0.18 * U(rng));
            QRadialGradient g(pt - QPointF(rx * 0.3, ry * 0.35), qMax(rx, ry));
            QColor mid = dc; mid.setAlphaF(c.alphaF());
            QColor edge = dc.darker(150); edge.setAlphaF(c.alphaF());
            QColor hi = dc.lighter(200); hi.setAlphaF(c.alphaF());
            g.setColorAt(0, hi);
            g.setColorAt(0.28, mid.lighter(140));
            g.setColorAt(0.7, mid);
            g.setColorAt(1, edge);
            p.setBrush(g);
            p.setPen(Qt::NoPen);
            p.drawEllipse(pt, rx, ry);
            p.setBrush(QColor(255, 255, 255, int(c.alpha() * 0.55)));
            p.drawEllipse(pt - QPointF(rx * 0.35, ry * 0.4), rx * 0.16, ry * 0.12);
        });
        break;
    }
    case Look::NeonTube: {
        // Dark ink (e.g. black on a light canvas) must still read as a tube: lift it to a
        // neon hue rather than Plus-blending zeros or a white-on-white core.
        QColor src = c;
        if (src.lightness() < 50) {
            const int h = src.hue() < 0 ? 190 : src.hue();
            src = QColor::fromHsv(h, 255, 255, c.alpha());
        }
        QColor glow = src;
        glow.setAlphaF(float(qBound(0.2, src.alphaF() * 0.55, 0.8)));
        p.save();
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.setPen(qpen(glow, w * 2.4, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        QColor mid = src.lighter(130);
        mid.setAlphaF(float(qBound(0.0, src.alphaF() * 0.7, 1.0)));
        p.setPen(qpen(mid, w * 1.15, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        p.restore();
        QColor core = src.lighter(170);
        if (core.lightness() > 230) core = QColor::fromHsv(src.hue(), qMax(40, src.saturation() - 60), 220, src.alpha());
        varyStroke(path, core, qMax(1.0, w * 0.32), Qt::RoundCap, Qt::RoundJoin);
        // A slightly darker envelope so the tube stays visible on a light background.
        QColor rim = src.darker(140);
        rim.setAlphaF(float(qBound(0.0, src.alphaF() * 0.85, 1.0)));
        varyStroke(path, rim, qMax(0.8, w * 0.14), Qt::RoundCap, Qt::RoundJoin);
        break;
    }
    case Look::Gouache: {
        const double r = qMax(0.8, w / 2);
        walk(path, spaceStep(d, qMax(0.6, r * 0.45)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double j = (std::fmod(std::sin((dist + in.seed) * 12.9898) * 43758.5453, 1.0));
            const double jr = r * (0.88 + 0.16 * j);
            QColor gc = c;
            gc.setAlphaF(float(qBound(0.0, c.alphaF() * (0.92 + 0.08 * (1 - j)), 1.0))); // matte, nearly opaque
            // Punch dry-edge bites on this dab only, then SourceOver it — DestinationOut
            // on the shared painter would erase earlier dabs of the same stroke.
            const int pad = int(std::ceil(jr * 2)) + 2;
            QImage dab(pad * 2, pad * 2, QImage::Format_ARGB32_Premultiplied);
            dab.fill(0);
            {
                QPainter dp(&dab);
                dp.setRenderHint(QPainter::Antialiasing, p.renderHints() & QPainter::Antialiasing);
                dp.translate(pad, pad);
                dp.rotate(std::atan2(dir.y(), dir.x()) * 180 / M_PI);
                dp.setPen(Qt::NoPen);
                dp.setBrush(gc);
                if (d.square) dp.drawRoundedRect(QRectF(-jr * 0.9, -jr * 0.75, jr * 1.8, jr * 1.5), jr * 0.25, jr * 0.25);
                else dp.drawEllipse(QPointF(0, 0), jr, jr * 0.85);
                dp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                const int bites = 1 + int(j * 3);
                for (int b = 0; b < bites; ++b) {
                    const double a = (j + b * 0.37 + in.seed * 0.001) * 2 * M_PI;
                    dp.setBrush(QColor(0, 0, 0, 180));
                    dp.drawEllipse(QPointF(std::cos(a), std::sin(a)) * jr * 0.95, jr * 0.18, jr * 0.14);
                }
            }
            p.drawImage(QPointF(pt.x() - pad, pt.y() - pad), dab);
        });
        break;
    }
    case Look::InkWash: {
        const double rMax = qMax(0.8, w / 2) * 1.3;
        // soft grey halo
        walk(path, spaceStep(d, qMax(0.8, rMax * 0.5)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double press = qBound(0.15, in.pressureAt(dist), 1.0);
            const double r = qMax(0.8, w / 2) * (0.7 + 0.6 * press);
            QColor halo = c;
            const int grey = qBound(0, (c.red() + c.green() + c.blue()) / 3, 180);
            halo = QColor(grey, grey, grey);
            halo.setAlphaF(float(qBound(0.0, c.alphaF() * 0.12 * press, 1.0)));
            QRadialGradient g(pt, r * 2.1);
            g.setColorAt(0, halo);
            QColor z = halo; z.setAlpha(0);
            g.setColorAt(1, z);
            p.setBrush(g);
            p.drawEllipse(pt, r * 2.1, r * 2.1);
        });
        walk(path, spaceStep(d, qMax(0.6, rMax * 0.35)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double press = qBound(0.15, in.pressureAt(dist), 1.0);
            const double slow = qBound(0.0, 1.0 - in.speedAt(dist), 1.0);
            const double r = qMax(0.8, w / 2) * (0.7 + 0.6 * press);
            const double pool = slow * 0.5 + 0.15 * (0.5 + 0.5 * std::sin(dist * 0.15 + in.seed));
            QColor ink = c.darker(110);
            ink.setAlphaF(float(qBound(0.0, c.alphaF() * (0.45 + 0.5 * press) * (d.inkAmount / 100.0), 1.0)));
            QRadialGradient g(pt, r * (1.0 + pool));
            g.setColorAt(0, ink);
            QColor rim = ink.darker(130);
            rim.setAlphaF(float(qBound(0.0, ink.alphaF() * 0.8, 1.0)));
            QColor z = ink; z.setAlpha(0);
            g.setColorAt(0.65, rim); // pooling wet edge
            g.setColorAt(1, z);
            p.setBrush(g);
            p.drawEllipse(pt, r * (1.0 + pool), r * (1.0 + pool));
            if (press > 0.55) { // bleed
                QColor bl = ink;
                bl.setAlphaF(float(qBound(0.0, ink.alphaF() * 0.25, 1.0)));
                const double a = std::fmod(dist * 0.73 + in.seed, 2 * M_PI);
                p.setBrush(bl);
                p.drawEllipse(pt + QPointF(std::cos(a), std::sin(a)) * r * 0.7, r * 0.55, r * 0.7);
            }
        });
        break;
    }
    case Look::PixelPencil: {
        QPainterPath snapped;
        for (const QPolygonF &poly : path.toSubpathPolygons()) {
            if (poly.isEmpty()) continue;
            snapped.moveTo(QPointF(qRound(poly[0].x()), qRound(poly[0].y())));
            for (int i = 1; i < poly.size(); ++i)
                snapped.lineTo(QPointF(qRound(poly[i].x()), qRound(poly[i].y())));
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        varyStroke(snapped, c, 1, Qt::SquareCap, Qt::MiterJoin);
        break;
    }
    case Look::RibbonTwist: {
        const double step = qMax(1.0, w * 0.25);
        QPointF prev;
        double prevHalf = 0;
        bool have = false;
        walk(path, spaceStep(d, step), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double angR = sAng(0, pt, dist) * M_PI / 180.0;
            const double ca = std::cos(angR), sa = std::sin(angR);
            QPointF n(-dir.y(), dir.x());
            n = QPointF(n.x() * ca - n.y() * sa, n.x() * sa + n.y() * ca);
            const double twist = dist / qMax(2.0, w) * 0.85;
            const double half = (w / 2) * std::cos(twist) * sKsz(pt, dist);
            if (have) {
                const QPointF a1 = prev + n * prevHalf, a2 = prev - n * prevHalf;
                const QPointF b1 = pt + n * half, b2 = pt - n * half;
                const double sh = 0.5 + 0.5 * std::sin(twist);
                QColor lit = c.lighter(140), dk = c.darker(140);
                lit.setAlphaF(float(qBound(0.0, c.alphaF(), 1.0))); dk.setAlphaF(float(qBound(0.0, c.alphaF(), 1.0)));
                QLinearGradient g(a1, a2);
                g.setColorAt(0, lit);
                g.setColorAt(1, dk);
                p.setBrush(g);
                p.setPen(Qt::NoPen);
                p.drawPolygon(QPolygonF({a1, b1, b2, a2}));
                // flip crease
                if (prevHalf * half < 0) {
                    p.setPen(qpen(c.darker(160), qMax(1.0, w * 0.08), Qt::RoundCap, Qt::RoundJoin));
                    p.drawLine(prev, pt);
                    p.setPen(Qt::NoPen);
                }
                Q_UNUSED(sh);
            }
            prev = pt; prevHalf = half; have = true;
        });
        break;
    }
    case Look::Chain: {
        const double step = qMax(2.0, w * 1.05);
        const double spacing = spaceStep(d, step);
        walk(path, spacing, [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            p.save();
            p.translate(pt);
            const int i = int(std::llround(dist / qMax(0.25, spacing)));
            p.rotate(std::atan2(dir.y(), dir.x()) * 180 / M_PI + (i % 2 ? 90 : 0));
            QPen pn = qpen(c, qMax(1.0, w * 0.18), Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pn);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(0, 0), w * 0.62, w * 0.32);
            p.restore();
        });
        break;
    }
    case Look::Rope: {
        const int strands = qMax(2, d.tips);
        for (int s = 0; s < strands; ++s) {
            QPolygonF line;
            QVector<double> dists;
            walk(path, spaceStep(d, qMax(0.8, w * 0.2)), [&](QPointF pt, QPointF dir, double dist) {
                const QPointF n(-dir.y(), dir.x());
                const double off = std::sin(dist / qMax(2.0, w) * 2.2 + s * 2 * M_PI / strands) * w * 0.38 * sKsz(pt, dist);
                line << pt + n * off;
                dists << dist;
            });
            p.setBrush(Qt::NoBrush);
            for (int i = 0; i + 1 < line.size(); ++i) {
                QColor sc = sCol(cOrig, line[i], dists[i]);
                sc = (s % 2) ? sc.darker(125) : sc.lighter(115);
                sc.setAlphaF(float(qBound(0.0, sCol(cOrig, line[i], dists[i]).alphaF(), 1.0)));
                p.setPen(qpen(sc, qMax(0.6, w / (strands + 0.5) * sKsz(line[i], dists[i])),
                              Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(line[i], line[i + 1]);
            }
        }
        break;
    }
    case Look::Stitch: {
        const double on = qMax(2.0, w * 1.1), off = qMax(2.0, w * 1.4);
        const double period = on + off;
        walk(path, spaceStep(d, 1.0), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double ph = std::fmod(dist, period);
            if (ph > on) return;
            const QPointF n(-dir.y(), dir.x());
            const QPointF a = pt - dir * 0.5, b = pt + dir * 0.5;
            // only draw once per stitch: at the start of the on-run
            if (ph > 1.0) return;
            const QPointF s0 = pt, s1 = pt + dir * on;
            p.setPen(qpen(c.darker(160), qMax(1.0, w * 0.22), Qt::FlatCap, Qt::MiterJoin));
            p.drawLine(s0 + n * (w * 0.18 + 1), s1 + n * (w * 0.18 + 1)); // shadow
            p.setPen(qpen(c, qMax(1.0, w * 0.22), Qt::FlatCap, Qt::MiterJoin));
            p.drawLine(s0 + n * (w * 0.05), s1 + n * (w * 0.05));
            Q_UNUSED(a); Q_UNUSED(b);
        });
        break;
    }
    case Look::Glitter: {
        walk(path, spaceStep(d, qMax(1.2, w * 0.45)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const int n = 2 + int(std::fmod(std::abs(std::sin((dist + 1) * (in.seed + 3))), 1.0) * 4);
            std::mt19937 gr = rngAt(in.seed, dist, 17);
            for (int i = 0; i < n; ++i) {
                const double a = U(gr) * 2 * M_PI, rr = (w * 0.7) * std::sqrt(U(gr));
                const QPointF q = pt + QPointF(std::cos(a), std::sin(a)) * rr;
                int h, s, l, al;
                c.getHsl(&h, &s, &l, &al);
                if (h < 0) h = 0;
                h = (h + int((U(gr) - 0.5) * 40) + 360) % 360;
                QColor gc = QColor::fromHsl(h, qMin(255, s + 20), qBound(80, l + int(U(gr) * 80), 255), al);
                if (U(gr) > 0.78) gc = QColor(255, 255, 255, al);
                const double sz = qMax(0.6, w * (0.08 + 0.18 * U(gr)));
                p.setBrush(gc);
                p.setPen(Qt::NoPen);
                if (U(gr) > 0.55) {
                    QPolygonF dia;
                    dia << q + QPointF(0, -sz) << q + QPointF(sz, 0) << q + QPointF(0, sz) << q + QPointF(-sz, 0);
                    p.drawPolygon(dia);
                } else p.drawEllipse(q, sz * 0.7, sz * 0.7);
            }
        });
        // faint core
        QColor core = c; core.setAlphaF(float(qBound(0.0, c.alphaF() * 0.35, 1.0)));
        varyStroke(path, core, qMax(0.8, w * 0.25), Qt::RoundCap, Qt::RoundJoin);
        break;
    }
    case Look::Smoke: {
        for (int trail = 0; trail < 4; ++trail) {
            QPolygonF line;
            QVector<double> dists;
            walk(path, spaceStep(d, qMax(0.8, w * 0.3)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
                const QPointF n(-dir.y(), dir.x());
                const double turb = std::sin(dist * 0.21 + trail * 1.7 + (in.seed % 97) * 0.1)
                                    + 0.45 * std::sin(dist * 0.53 + trail);
                line << pt + n * (turb * w * 0.9) + QPointF(0, -dist * 0.04 * (trail + 1));
                dists << dist;
            });
            if (line.size() < 2) continue;
            const double fadeLen = qMax(1.0, w * 10.0);
            for (int i = 0; i + 1 < line.size(); ++i) {
                // Distance from the start, not i/N, so a live tail matches the finished stroke.
                const double fade = qBound(0.0, 1.0 - dists[i] / fadeLen, 1.0);
                QColor sc = sCol(cOrig, line[i], dists[i]).lighter(140);
                sc.setAlphaF(float(qBound(0.0, sc.alphaF() * 0.14 * fade * (1.0 - trail * 0.15) * inkScale(d), 1.0)));
                p.setPen(qpen(sc, qMax(0.8, w * (1.1 - trail * 0.15) * fade * sKsz(line[i], dists[i])), Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(line[i], line[i + 1]);
            }
        }
        break;
    }
    case Look::Fire: {
        walk(path, spaceStep(d, qMax(1.2, w * 0.55)), [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 fr(in.seed + quint32(dist * 13));
            const int tongues = 1 + int(U(fr) * 3);
            for (int t = 0; t < tongues; ++t) {
                const double rise = w * (1.2 + 2.2 * U(fr));
                const double lean = (U(fr) - 0.5) * w * 1.4 + dir.x() * w * 0.2;
                const QPointF tip = pt + QPointF(lean, -rise);
                const QPointF c1 = pt + QPointF(lean * 0.3 - w * 0.15, -rise * 0.4);
                const QPointF c2 = pt + QPointF(lean * 0.8 + w * 0.1, -rise * 0.75);
                QPainterPath tongue;
                tongue.moveTo(pt + QPointF(-w * 0.25, 0));
                tongue.quadTo(c1, tip);
                tongue.quadTo(c2, pt + QPointF(w * 0.25, 0));
                QLinearGradient g(pt, tip);
                QColor base = c, mid = QColor::fromHsv(qBound(0, 25 + c.hue() % 20, 45), 255, 255, c.alpha());
                QColor tipC = QColor(255, 245, 140, int(c.alpha() * 0.85));
                g.setColorAt(0, base);
                g.setColorAt(0.45, mid);
                g.setColorAt(1, tipC);
                p.setBrush(g);
                p.setPen(Qt::NoPen);
                p.drawPath(tongue);
            }
        });
        break;
    }
    case Look::Rainbow: {
        walk(path, spaceStep(d, qMax(0.5, w * 0.2)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            const double hueK = senseMul(d.hueSens, in, pt, dist, sBounds);
            const double satK = senseMul(d.satSens, in, pt, dist, sBounds);
            const int h = int(std::fmod(dist * 2.0 + (hueK - 1.0) * 90.0 + 3600.0, 360.0));
            QColor rc = QColor::fromHsv(h, qBound(0, int(std::lround(230 * satK)), 255), 255, c.alpha());
            p.setBrush(rc);
            p.setPen(Qt::NoPen);
            p.drawEllipse(pt, w / 2, w / 2);
        });
        break;
    }
    case Look::Lightning: {
        auto jagged = [&](QPolygonF base, const QVector<double> &baseDist, int depth, quint32 seed) {
            QVector<QPolygonF> out;
            std::function<void(QPolygonF, QVector<double>, int, quint32)> rec;
            rec = [&](QPolygonF line, QVector<double> dists, int depthLeft, quint32 salt) {
                if (line.size() < 2) return;
                QPolygonF jag;
                QVector<double> jagD;
                jag << line.first();
                jagD << (dists.isEmpty() ? 0.0 : dists.first());
                for (int i = 0; i + 1 < line.size(); ++i) {
                    const QPointF a = line[i], b = line[i + 1];
                    const QPointF mid = (a + b) / 2;
                    const QPointF dir = b - a;
                    const double len = std::hypot(dir.x(), dir.y());
                    const QPointF n = len > 1e-9 ? QPointF(-dir.y() / len, dir.x() / len) : QPointF(0, -1);
                    const double d0 = (i < dists.size()) ? dists[i] : 0.0;
                    const double d1 = (i + 1 < dists.size()) ? dists[i + 1] : d0;
                    const double midDist = 0.5 * (d0 + d1);
                    std::mt19937 lr = rngAt(seed, midDist, salt ^ quint32(depthLeft + 1));
                    jag << mid + n * ((U(lr) - 0.5) * qMax(2.0, w * 1.1));
                    jagD << midDist;
                    jag << b;
                    jagD << d1;
                }
                if (depthLeft <= 0) { out << jag; return; }
                rec(jag, jagD, depthLeft - 1, salt * 1664525u + 1013904223u);
                // occasional fork, keyed by the same mid-run seed so a suffix matches
                std::mt19937 fr = rngAt(seed, jagD.isEmpty() ? 0.0 : jagD[qMin(1, jagD.size() - 1)], salt ^ 0x85ebca6bu);
                if (U(fr) > 0.55 && jag.size() > 3) {
                    const int at = 1 + int(U(fr) * (jag.size() - 2));
                    const QPointF o = jag[at];
                    const QPointF dir = jag[qMin(at + 1, jag.size() - 1)] - o;
                    const double len = std::hypot(dir.x(), dir.y());
                    const QPointF n = len > 1e-9 ? QPointF(-dir.y() / len, dir.x() / len) : QPointF(1, 0);
                    QPolygonF fork;
                    QVector<double> forkD;
                    const double od = (at < jagD.size()) ? jagD[at] : 0.0;
                    fork << o << o + dir * 0.6 + n * w * (U(fr) - 0.5) * 4 << o + dir * 1.1 + n * w * 2 * (U(fr) > 0.5 ? 1 : -1);
                    forkD << od << od + len * 0.6 << od + len * 1.1;
                    rec(fork, forkD, depthLeft - 1, salt ^ 0x9e3779b9u);
                }
            };
            rec(base, baseDist, depth, 1u);
            return out;
        };
        QPolygonF spine;
        QVector<double> spineDist;
        walk(path, spaceStep(d, qMax(2.0, w * 0.8)), [&](QPointF pt, QPointF, double dist) { spine << pt; spineDist << dist; });
        p.setBrush(Qt::NoBrush);
        const double pathLen = spineDist.isEmpty() ? 1.0 : qMax(1.0, spineDist.last());
        for (const QPolygonF &jag : jagged(spine, spineDist, 2, in.seed)) {
            if (jag.size() < 2) continue;
            for (int i = 0; i + 1 < jag.size(); ++i) {
                const double dist = pathLen * double(i) / qMax(1, jag.size() - 1);
                const QColor cc = sCol(cOrig, jag[i], dist);
                QColor glow = cc.lighter(180);
                glow.setAlphaF(float(qBound(0.0, cc.alphaF() * 0.35, 1.0)));
                const double sk = sKsz(jag[i], dist);
                p.setPen(qpen(glow, qMax(1.5, w * 0.7 * sk), Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(jag[i], jag[i + 1]);
                p.setPen(qpen(QColor(255, 255, 255, cc.alpha()), qMax(0.8, w * 0.22 * sk), Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(jag[i], jag[i + 1]);
            }
        }
        break;
    }
    case Look::SprayCan: {
        if (d.sprayLook >= 1) {
            paintSprayAir(p, path, in, cOrig, w);
            break;
        }
        const double r = qMax(1.0, w * 0.7);
        const int n = qMin(280, qMax(4, int(r * r * 0.9)));
        const int dim = qMax(8, int(std::ceil(r * 2 + 8)));
        const QPointF origin(dim / 2.0, dim / 2.0);
        StampKey key;
        key.seed = in.seed;
        key.aux = 0x5CAu | (quint32(qRound(r * 100)) << 8) | (quint32(qBound(0, d.inkAmount, 100)) << 24);
        key.rgba = quint32(c.rgba());
        key.dim = dim;
        key.n = n;
        const StampAtlas &atlas = cachedStampAtlas(key, [&](StampAtlas &a) {
            for (int v = 0; v < kStampVariants; ++v) {
                a.img[v] = QImage(dim, dim, QImage::Format_ARGB32_Premultiplied);
                a.img[v].fill(0);
                QPainter sp(&a.img[v]);
                sp.setRenderHint(QPainter::Antialiasing, true);
                sp.setPen(Qt::NoPen);
                std::mt19937 sr(in.seed + quint32(v + 1) * 2246822519u);
                std::uniform_real_distribution<double> SU(0, 1);
                for (int i = 0; i < n; ++i) {
                    const double a = SU(sr) * 2 * M_PI, rr = r * std::pow(SU(sr), 0.45);
                    QColor dc = c;
                    dc.setAlphaF(float(qBound(0.0, c.alphaF() * (0.25 + 0.6 * SU(sr)) * (d.inkAmount / 100.0), 1.0)));
                    sp.setBrush(dc);
                    const double dot = 0.35 + SU(sr) * 0.7;
                    sp.drawEllipse(origin + QPointF(std::cos(a), std::sin(a)) * rr, dot, dot);
                }
            }
        });
        walk(path, spaceStep(d, qMax(0.8, r * 0.4)), [&](QPointF pt, QPointF, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 sr = rngAt(in.seed, dist, 29);
            const int v = int(U(sr) * kStampVariants) % kStampVariants;
            p.setPen(Qt::NoPen);
            drawTintedStamp(p, atlas.img[v], pt - origin, cOrig, c);
            const bool slow = in.speedAt(dist) < 0.55;
            if (slow && U(sr) > 0.45) {
                const double len = w * (0.8 + 2.5 * U(sr)) * (1.1 - in.speedAt(dist));
                QColor drip = c;
                drip.setAlphaF(float(qBound(0.0, c.alphaF() * 0.85, 1.0)));
                p.setPen(qpen(drip, qMax(0.8, w * 0.18), Qt::RoundCap, Qt::RoundJoin));
                p.drawLine(pt, pt + QPointF((U(sr) - 0.5) * w * 0.2, len));
                p.setPen(Qt::NoPen);
                p.setBrush(drip);
                p.drawEllipse(pt + QPointF(0, len), w * 0.16, w * 0.2);
            }
        });
        break;
    }
    case Look::ImageStamp: {
        QImage stamp = in.stampImage;
        if (stamp.isNull()) stamp = loadStampImage(d.stampSource);
        if (stamp.isNull()) {
            const int s = 32;
            stamp = QImage(s, s, QImage::Format_ARGB32_Premultiplied);
            stamp.fill(0);
            QPainter sp(&stamp);
            sp.setRenderHint(QPainter::Antialiasing);
            sp.setBrush(c);
            sp.setPen(Qt::NoPen);
            QPolygonF star;
            for (int i = 0; i < 10; ++i) {
                const double a = i * M_PI / 5 - M_PI / 2;
                const double rr = (i % 2) ? s * 0.18 : s * 0.46;
                star << QPointF(s / 2.0 + rr * std::cos(a), s / 2.0 + rr * std::sin(a));
            }
            sp.drawPolygon(star);
        }
        const double step = qMax(2.0, w * (d.spacing / 100.0) * 1.2);
        walk(path, step, [&](QPointF pt, QPointF dir, double dist) {
            const QColor c = sCol(cOrig, pt, dist);
            const SenseDab _senseDab(p, pt, sKsz(pt, dist));
            std::mt19937 sr = rngAt(in.seed, dist, 11);
            const double szJ = 0.75 + U(sr) * 0.5;
            const double angJ = (U(sr) - 0.5) * 40;
            const double ang = std::atan2(dir.y(), dir.x()) * 180 / M_PI + angJ;
            const double sc = (w / qMax(1, stamp.width())) * 1.6 * szJ;
            p.save();
            p.translate(pt);
            p.rotate(ang);
            p.scale(sc, sc);
            p.translate(-stamp.width() / 2.0, -stamp.height() / 2.0);
            p.setOpacity(c.alphaF() * (d.inkAmount / 100.0));
            if (senseVaries(d.satSens, in) || senseVaries(d.hueSens, in)) {
                QImage tinted = stamp.convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < tinted.height(); ++y) {
                    auto *row = reinterpret_cast<QRgb *>(tinted.scanLine(y));
                    for (int x = 0; x < tinted.width(); ++x) {
                        const int a = qAlpha(row[x]);
                        if (!a) continue;
                        QColor px = QColor::fromRgba(row[x]);
                        const int aa = px.alpha();
                        px.setAlpha(255);
                        px = senseSatHue(px, d, in, pt, dist, sBounds);
                        px.setAlpha(aa);
                        row[x] = px.rgba();
                    }
                }
                p.drawImage(0, 0, tinted);
            } else {
                p.drawImage(0, 0, stamp);
            }
            p.restore();
        });
        break;
    }
    case Look::Cellular:
        paintCellular(p, path, in);
        break;
    }
    p.restore();
}


double sampleAt(const QVector<double> &dist, const QVector<double> &val, double d, double fallback)
{
    if (dist.isEmpty() || val.size() != dist.size()) return fallback;
    if (d <= dist.first()) return val.first();
    if (d >= dist.last()) return val.last();
    int i = 1;
    while (i < dist.size() && dist[i] < d) ++i;
    const double t = (d - dist[i - 1]) / qMax(1e-9, dist[i] - dist[i - 1]);
    return val[i - 1] + (val[i] - val[i - 1]) * t;
}
double PaintInput::pressureAt(double dist) const { return sampleAt(sampleDist, samplePressure, dist, pressure); }
double PaintInput::speedAt(double dist) const { return sampleAt(sampleDist, sampleSpeed, dist, speed); }
double PaintInput::timeAt(double dist) const { return sampleAt(sampleDist, sampleTime, dist, -1.0); }

QImage loadStampImage(const QString &source)
{
    if (source.isEmpty()) return QImage();
    qint64 token = 0;
    if (source.startsWith(QLatin1String("symbol:"))) {
        bool ok = false;
        const quint64 id = source.mid(int(QStringLiteral("symbol:").size())).toULongLong(&ok);
        if (ok) {
            const QByteArray dig = render::symbolBodyDigest(id);
            token = qint64(qHash(dig));
        }
    } else {
        const QFileInfo fi(source);
        token = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
    }
    static QMutex mu;
    // Cost is KiB; 64 MiB bounds memory instead of a slot count of 8.
    static QCache<QString, QImage> cache(64 * 1024);
    const QString ckey = source + QLatin1Char(0x1f) + QString::number(token);
    {
        QMutexLocker lock(&mu);
        if (QImage *hit = cache.object(ckey))
            return *hit;
    }
    QImage im;
    constexpr int kStampCap = 256;
    if (source.startsWith(QLatin1String("symbol:"))) {
        static thread_local int depth = 0;
        bool ok = false;
        const quint64 id = source.mid(int(QStringLiteral("symbol:").size())).toULongLong(&ok);
        if (!ok || depth > 3) {
            QMutexLocker lock(&mu);
            cache.insert(ckey, new QImage(), 1);
            return QImage();
        }
        const Symbol *sym = findSymbol(id);
        if (!sym || sym->frames.isEmpty()) {
            QMutexLocker lock(&mu);
            cache.insert(ckey, new QImage(), 1);
            return QImage();
        }
        const QRectF b = sym->visualBounds();
        QRect area = b.toAlignedRect().adjusted(-1, -1, 1, 1);
        if (area.width() < 1 || area.height() < 1) {
            QMutexLocker lock(&mu);
            cache.insert(ckey, new QImage(), 1);
            return QImage();
        }
        ++depth;
        im = render::renderObjects(sym->frames.first(), area);
        --depth;
        if (im.isNull()) {
            QMutexLocker lock(&mu);
            cache.insert(ckey, new QImage(), 1);
            return im;
        }
        if (im.width() > kStampCap || im.height() > kStampCap)
            im = im.scaled(kStampCap, kStampCap, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        im = QImage(source);
        if (im.isNull()) {
            QMutexLocker lock(&mu);
            cache.insert(ckey, new QImage(), 1);
            return im;
        }
        im = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (im.width() > kStampCap || im.height() > kStampCap)
            im = im.scaled(kStampCap, kStampCap, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const int cost = qMax(1, int((im.sizeInBytes() + 1023) / 1024));
    QMutexLocker lock(&mu);
    cache.insert(ckey, new QImage(im), cost);
    return im;
}

void releaseUnderSampleCache()
{
    clearUnderCache();
}

QPainterPath previewCurve(const QRectF &inner)
{
    QPainterPath path;
    if (inner.width() <= 1 || inner.height() <= 1)
        return path;
    const double x0 = inner.left(), x1 = inner.right();
    const double y0 = inner.top(), y1 = inner.bottom();
    const double w = inner.width(), h = inner.height();
    path.moveTo(x0, y0 + h * 0.65);
    path.cubicTo(x0 + w * 0.25, y0 + h * 0.05,
                 x0 + w * 0.40, y0 + h * 0.05,
                 x0 + w * 0.50, y0 + h * 0.50);
    path.cubicTo(x0 + w * 0.60, y1 - h * 0.05,
                 x0 + w * 0.75, y1 - h * 0.05,
                 x1, y0 + h * 0.35);
    Q_UNUSED(y1);
    return path;
}

PreviewLayout previewLayout(const Params &p, double width, const QSize &dest)
{
    PreviewLayout L;
    const double w = qMax(0.5, width);
    const double rad = qMax(1.0, paintedRadius(p, w));
    const int pathBoxW = qMax(qMax(dest.width(), 1), 400);
    const int pathBoxH = qMax(qMax(dest.height(), 1), 80);
    const int maxW = 1600, maxH = 800;
    double s = 1.0;
    const double needW = pathBoxW + 2 * rad + 8;
    const double needH = pathBoxH + 2 * rad + 8;
    if (needW > maxW || needH > maxH)
        s = qMax(0.05, qMin(maxW / needW, maxH / needH));
    const int margin = qMax(4, int(std::ceil(rad * s)) + 2);
    L.scale = s;
    L.margin = margin;
    L.work = QSize(qMax(8, int(std::ceil(pathBoxW * s)) + 2 * margin),
                   qMax(8, int(std::ceil(pathBoxH * s)) + 2 * margin));
    if (L.work.width() > 2 * margin && L.work.height() > 2 * margin)
        L.path = previewCurve(QRectF(margin, margin, L.work.width() - 2 * margin, L.work.height() - 2 * margin));
    return L;
}

QSize previewWorkSize(const Params &p, double width, const QSize &dest)
{
    return previewLayout(p, width, dest).work;
}

QImage fitPreview(const QImage &painted, const QSize &dest, const QColor &background, int pad)
{
    QImage out(dest, QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    if (painted.isNull() || dest.width() <= 0 || dest.height() <= 0)
        return out;
    const QImage src = painted.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int x0 = src.width(), y0 = src.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *sl = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            if (qAlpha(sl[x]) > 10) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0) return out;
    const int p = qMax(0, pad);
    QRect tight(QPoint(x0, y0), QPoint(x1, y1));
    tight.adjust(-4, -4, 4, 4);
    tight = tight.intersected(src.rect());
    const QImage crop = src.copy(tight);
    const int innerW = qMax(1, dest.width() - 2 * p);
    const int innerH = qMax(1, dest.height() - 2 * p);
    double sx = double(innerW) / qMax(1, crop.width());
    double sy = double(innerH) / qMax(1, crop.height());
    double sc = qMin(sx, sy);
    // Keep a little room; don't upscale a hairline more than 8x (still visible, not a blurry block).
    if (sc > 8.0) sc = 8.0;
    const int dw = qMax(1, int(std::lround(crop.width() * sc)));
    const int dh = qMax(1, int(std::lround(crop.height() * sc)));
    const QImage scaled = crop.scaled(dw, dh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPainter qp(&out);
    qp.setRenderHint(QPainter::SmoothPixmapTransform, true);
    qp.drawImage((dest.width() - dw) / 2, (dest.height() - dh) / 2, scaled);
    qp.end();
    return out;
}

void fillPreviewSenseRamp(PaintInput &in, const QPainterPath &path)
{
    const double len = qMax(1.0, path.length());
    in.sampleDist = {0.0, len};
    in.samplePressure = {0.15, 1.0};
    in.sampleSpeed = {1.35, 0.40};
}

static PaintInput previewInput(const PaintInput &in)
{
    PaintInput o = in;
    if (o.width <= 0) o.width = 0.5;
    if (!o.seed) o.seed = kPreviewSeed;
    if (o.params.look == Look::ImageStamp && o.stampImage.isNull())
        o.stampImage = loadStampImage(o.params.stampSource);
    return o;
}

QImage renderPreviewRaw(const PaintInput &in0, const QSize &dest)
{
    const PaintInput in = previewInput(in0);
    const PreviewLayout L = previewLayout(in.params, in.width, dest);
    QImage img(L.work, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    if (L.path.isEmpty()) return img;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    PaintInput pin = in;
    pin.width = in.width * L.scale;
    if (pin.sampleDist.isEmpty())
        fillPreviewSenseRamp(pin, L.path);
    paint(p, L.path, pin);
    p.end();
    return img;
}

QImage renderPreview(const PaintInput &in, const QSize &dest, const QColor &background)
{
    return fitPreview(renderPreviewRaw(in, dest), dest, background);
}

} // namespace stroke
