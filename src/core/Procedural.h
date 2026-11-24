#pragma once
#include "Object.h"
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>

// Procedural textures: seedable, tileable noise, cellular and geometric generators with a colour
// ramp. Used as a fill/stroke Texture source, as a Pattern fill and by Filters > Render > Texture.
// Parameters are stored (as compact JSON) on the object so the fill stays editable.
namespace proc {

struct Params {
    QString gen = QStringLiteral("perlin");
    quint32 seed = 1;
    int width = 128, height = 128;  // tile size, px
    bool tileable = true;
    double scale = 4;           // features across the tile (noise period, cells, veins, ...)
    double contrast = 0;        // -100..100
    double brightness = 0;      // -100..100
    bool invert = false;
    // noise
    int basis = 0;              // 0 Perlin, 1 Simplex (fBm / turbulence / ridged / warp / marble / wood / clouds)
    int octaves = 4;
    double persistence = 0.5;
    double lacunarity = 2.0;
    double warp = 1.0;          // domain warp / vein turbulence strength
    // Worley: 0 F1, 1 F2, 2 F2-F1, 3 crackle, 4 stones, 5 cells; patterns: see patternNames()
    int variant = 0;
    int metric = 0;             // 0 Euclidean, 1 Manhattan, 2 Chebyshev
    double width2 = 0.5;        // line / dot / duty width for patterns (0..1)
    // cellular
    int cell = 2;               // px per cell
    int generations = 60;
    double density = 0.4;
    int rule = 30;              // elementary CA (Wolfram code)
    bool singleCell = false;
    QString lifeRule = QStringLiteral("B3/S23");
    int states = 14, threshold = 1, range = 1;
    bool moore = false;         // cyclic CA neighbourhood
    double feed = 0.0367, kill = 0.0649;   // Gray-Scott (mitosis)
    QVector<GradStop> ramp;     // empty = black -> white
    QVector<QColor> stateColors; // per-state colours (Brian's Brain: off, firing, refractory)

    bool operator==(const Params &o) const;
};

QStringList generatorKeys();
QString generatorLabel(const QString &key);
QStringList presetNames(const QString &gen);
bool applyPreset(Params &p, const QString &preset);   // keeps seed, size, ramp
QStringList worleyVariants();
QStringList patternNames();
QStringList lifePresets();      // "Conway", ...
QString lifePresetRule(const QString &name);
// Which parameters the generator uses (UI shows only these): "noise", "octaves", "warp",
// "worley", "pattern", "cell", "generations", "density", "rule", "life", "cyclic", "rd", "states"
QStringList usedParams(const QString &gen);

QJsonObject toJson(const Params &p);
Params fromJson(const QJsonObject &o);
QString toString(const Params &p);      // compact JSON
Params fromString(const QString &s);

// The raw field: values 0..1 (w*h), plus integer states for the cellular generators.
struct Field {
    int w = 0, h = 0;
    QVector<float> v;
    QVector<uchar> st;
    int nStates = 0;
};
Field generate(const Params &p, int w, int h, const std::atomic<bool> *cancel = nullptr);
// Coloured image (ARGB32 premultiplied) at w x h (defaults to the tile size).
QImage render(const Params &p, int w = 0, int h = 0, const std::atomic<bool> *cancel = nullptr);
QImage renderGray(const Params &p, int w = 0, int h = 0);    // Format_Grayscale8 luminance
// Cached tiles for fills: Pattern (colour) and Texture (grayscale)
QImage patternTile(const QString &json);
QImage textureTile(const QString &json);

// Labelled grid of every generator / preset (docs/textures.png). Needs a QGuiApplication.
QImage contactSheet(int tile = 144, int columns = 8);

// ---- building blocks (exposed for tests)
double perlin(double x, double y, int px, int py, quint32 seed);      // periodic when px,py > 0; ~[-1,1]
double simplex(double x, double y, quint32 seed);                     // ~[-1,1]
QVector<QVector<uchar>> elementary(int rule, int width, int rows, bool singleCell, quint32 seed, double density = 0.5);
struct LifeRule { quint16 born = 0, survive = 0; bool valid = false; };
LifeRule parseLifeRule(const QString &s);   // "B3/S23", "b36/s23", "B2/S"
QVector<uchar> lifeStep(const QVector<uchar> &g, int w, int h, const LifeRule &r);
// Brian's Brain: 0 off, 1 firing, 2 refractory (B2/S/C3)
QVector<uchar> briansBrainStep(const QVector<uchar> &g, int w, int h);
QVector<uchar> cyclicStep(const QVector<uchar> &g, int w, int h, int n, int threshold, int range, bool moore);
}
