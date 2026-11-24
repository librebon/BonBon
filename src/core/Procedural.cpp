#include "Procedural.h"
#include <functional>
#include <QPainter>
#include "Fills.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QThread>
#include <QThreadPool>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentMap>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace proc {
namespace {
constexpr double kPi = 3.14159265358979323846;

inline quint32 mix(quint32 h) {
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
    return h;
}
inline quint32 hash3(int x, int y, quint32 seed) {
    return mix(quint32(x) * 0x9E3779B1U ^ mix(quint32(y) * 0x85EBCA77U ^ mix(seed + 0x27d4eb2dU)));
}
inline double rnd01(int x, int y, quint32 seed) { return (hash3(x, y, seed) & 0xFFFFFF) / double(0x1000000); }
inline int wrapi(int a, int n) { return n > 0 ? ((a % n) + n) % n : a; }
inline double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
inline double smooth(double e0, double e1, double x) { double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3 - 2 * t); }
inline double fracd(double x) { return x - std::floor(x); }

// parallel row loop (QtConcurrent); body(y)
template <typename F> void rows(int h, F body, const std::atomic<bool> *cancel) {
    QVector<int> ys(h);
    std::iota(ys.begin(), ys.end(), 0);
    // a private pool: callers may themselves run on the global pool (live preview), and nesting
    // blocking maps on one pool can starve it
    static QThreadPool *pool = [] { auto *tp = new QThreadPool; tp->setMaxThreadCount(std::max(2, QThread::idealThreadCount())); return tp; }();
    QtConcurrent::blockingMap(pool, ys, [&](int y) { if (!cancel || !cancel->load()) body(y); });
}

// one noise octave at normalised coordinate (t,u) in [0,1) with `period` lattice cells
double basisNoise(const Params &p, double t, double u, double period, quint32 seed) {
    if (p.basis == 0) {
        if (p.tileable) {
            const int per = std::max(1, int(std::lround(period)));
            return perlin(t * per, u * per, per, per, seed);
        }
        return perlin(t * period, u * period, 0, 0, seed);
    }
    const double x = t * period, y = u * period;
    if (!p.tileable) return simplex(x, y, seed);
    // seamless simplex: blend the four periodic copies (bilinear weights over the tile)
    const double P = period, a = t, b = u;
    return simplex(x, y, seed) * (1 - a) * (1 - b) + simplex(x - P, y, seed) * a * (1 - b)
         + simplex(x, y - P, seed) * (1 - a) * b + simplex(x - P, y - P, seed) * a * b;
}

enum class Sum { Fbm, Turb, Ridged };
double octaveSum(const Params &p, double t, double u, Sum mode, quint32 seedOfs = 0, int octOverride = 0) {
    const int oct = octOverride > 0 ? octOverride : std::clamp(p.octaves, 1, 12);
    double amp = 1, total = 0, norm = 0, period = std::max(0.5, p.scale), weight = 1;
    for (int o = 0; o < oct; ++o) {
        const double n = basisNoise(p, t, u, period, p.seed * 7919u + seedOfs + quint32(o) * 1013u);
        if (mode == Sum::Fbm) total += n * amp;
        else if (mode == Sum::Turb) total += std::abs(n) * amp;
        else {   // Musgrave ridged multifractal
            double s = 1.0 - std::abs(n);
            s *= s;
            s *= weight;
            weight = std::clamp(s * 2.0, 0.0, 1.0);
            total += s * amp;
        }
        norm += amp;
        amp *= p.persistence;
        period *= p.lacunarity;
    }
    total /= std::max(1e-9, norm);
    return mode == Sum::Fbm ? total * 0.5 + 0.5 : total;
}

void normalise(QVector<float> &v) {
    if (v.isEmpty()) return;
    const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
    const float lo = *mn, span = *mx - *mn;
    if (span < 1e-9f) { std::fill(v.begin(), v.end(), 0.5f); return; }
    for (float &x : v) x = (x - lo) / span;
}

double metricDist(double dx, double dy, int m) {
    if (m == 1) return std::abs(dx) + std::abs(dy);
    if (m == 2) return std::max(std::abs(dx), std::abs(dy));
    return std::sqrt(dx * dx + dy * dy);
}

// ---- generators producing per-pixel values
Field pointwise(const Params &p, int w, int h, const std::atomic<bool> *cancel, double (*fn)(const Params &, double, double)) {
    Field f; f.w = w; f.h = h; f.v.resize(w * h);
    rows(h, [&](int y) {
        const double u = (y + 0.5) / h;
        float *line = f.v.data() + y * w;
        for (int x = 0; x < w; ++x) line[x] = float(fn(p, (x + 0.5) / w, u));
    }, cancel);
    return f;
}

double genFbm(const Params &p, double t, double u) { return octaveSum(p, t, u, Sum::Fbm); }
double genTurb(const Params &p, double t, double u) { return octaveSum(p, t, u, Sum::Turb); }
double genRidged(const Params &p, double t, double u) { return octaveSum(p, t, u, Sum::Ridged); }
double genWarp(const Params &p, double t, double u) {
    const double s = std::max(0.5, p.scale);
    const double qx = octaveSum(p, t, u, Sum::Fbm, 101), qy = octaveSum(p, t, u, Sum::Fbm, 202);
    const double k = p.warp * 2.0 / s;
    const double rx = octaveSum(p, t + k * (qx - 0.5) * 2, u + k * (qy - 0.5) * 2, Sum::Fbm, 303);
    const double ry = octaveSum(p, t + k * (qx - 0.5) * 2 + 0.17, u + k * (qy - 0.5) * 2 + 0.31, Sum::Fbm, 404);
    return octaveSum(p, t + k * (rx - 0.5) * 2, u + k * (ry - 0.5) * 2, Sum::Fbm);
}
double genMarble(const Params &p, double t, double u) {
    const double veins = std::max(1.0, std::round(p.scale / 2));
    const double turb = octaveSum(p, t, u, Sum::Turb);
    const double v = 0.5 + 0.5 * std::sin(2 * kPi * (t * veins + u * (p.tileable ? 0 : 0.3) + p.warp * turb * 1.5));
    return std::pow(v, 0.35);   // thin dark veins on light stone
}
double genWood(const Params &p, double t, double u) {
    // ring distance on the torus (periodic), disturbed by noise
    const double dx = std::sin(kPi * t), dy = std::sin(kPi * u) * 0.35;
    const double r = std::sqrt(dx * dx + dy * dy);
    const double n = octaveSum(p, t, u, Sum::Fbm);
    const double rings = std::max(1.0, p.scale * 2);
    const double g = fracd(r * rings + p.warp * n * 1.2);
    return 0.35 + 0.65 * std::pow(std::abs(std::sin(g * kPi)), 0.6) * (0.85 + 0.15 * n);
}
double genPerlin(const Params &p, double t, double u) { Params q = p; q.basis = 0; return octaveSum(q, t, u, Sum::Fbm); }
double genSimplex(const Params &p, double t, double u) { Params q = p; q.basis = 1; return octaveSum(q, t, u, Sum::Fbm); }

double genWorley(const Params &p, double t, double u) {
    const int n = std::max(1, int(std::lround(p.scale)));
    const double x = t * n, y = u * n;
    const int cx = int(std::floor(x)), cy = int(std::floor(y));
    double f1 = 1e9, f2 = 1e9;
    quint32 id1 = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) {
            const int gx = cx + i, gy = cy + j;
            const int hx = p.tileable ? wrapi(gx, n) : gx, hy = p.tileable ? wrapi(gy, n) : gy;
            const double px = gx + rnd01(hx, hy, p.seed * 31u + 1), py = gy + rnd01(hx, hy, p.seed * 31u + 2);
            const double d = metricDist(px - x, py - y, p.metric);
            if (d < f1) { f2 = f1; f1 = d; id1 = hash3(hx, hy, p.seed * 31u + 3); }
            else if (d < f2) f2 = d;
        }
    const double cellv = (id1 & 0xFFFF) / 65535.0;
    switch (p.variant) {
    case 1: return f2 * 0.8;
    case 2: return (f2 - f1) * 1.5;
    case 3: return smooth(0.0, 0.08 + 0.12 * p.width2, f2 - f1);                         // crackle: dark cracks
    case 4: return (0.35 + 0.65 * cellv) * smooth(0.0, 0.15 + 0.3 * p.width2, f2 - f1);   // stones
    case 5: return cellv;                                                                // flat cells
    default: return f1;
    }
}

double genPattern(const Params &p, double t, double u) {
    const int n = std::max(1, int(std::lround(p.scale)));
    const double x = t * n, y = u * n, fx = fracd(x), fy = fracd(y);
    const double wd = std::clamp(p.width2, 0.02, 0.98);
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double aa = 0.02;
    switch (p.variant) {
    case 0: return ((ix + iy) & 1) ? 1.0 : 0.0;                                            // checker
    case 1: { const double lw = wd * 0.3; return (fx < lw || fy < lw) ? 0.0 : 1.0; }       // grid
    case 2: return fx < wd ? 0.0 : 1.0;                                                   // stripes
    case 3: { const double d = std::hypot(fx - 0.5, fy - 0.5); return smooth(wd * 0.5 - aa, wd * 0.5 + aa, d); } // dots
    case 4: {                                                                             // brick
        const double by = y * 2, bfy = fracd(by);
        const int row = int(std::floor(by));
        const double bx = x + ((row & 1) ? 0.5 : 0.0), bfx = fracd(bx);
        const double m = wd * 0.12;
        return (bfy < m || bfx < m * 0.5) ? 0.15 : 0.85;
    }
    case 5: {                                                                             // hexagons
        const double sx = x, sy = y * 2.0 / std::sqrt(3.0) * 0.5 * 2;   // ~equal rows
        double best = 1e9, second = 1e9;
        const int rcy = int(std::floor(sy));
        for (int j = rcy - 1; j <= rcy + 1; ++j)
            for (int i = int(std::floor(sx)) - 1; i <= int(std::floor(sx)) + 1; ++i) {
                const double cxp = i + ((j & 1) ? 0.5 : 0.0), cyp = j;
                const double d = std::hypot((sx - cxp), (sy - cyp) * 0.866);
                if (d < best) { second = best; best = d; } else if (d < second) second = d;
            }
        return smooth(0.0, 0.05 + wd * 0.1, second - best);
    }
    case 6: {                                                                             // halftone
        Params q = p; q.scale = std::max(1.0, std::round(p.scale / 4)); q.octaves = 3;
        const double tone = octaveSum(q, (ix + 0.5) / n, (iy + 0.5) / n, Sum::Fbm);
        const double r = 0.62 * std::sqrt(std::clamp((tone - 0.15) / 0.7, 0.0, 1.0));
        const double d = std::hypot(fx - 0.5, fy - 0.5);
        return smooth(r - aa, r + aa, d);
    }
    case 7: {                                                                             // Truchet
        const int hx = p.tileable ? wrapi(ix, n) : ix, hy = p.tileable ? wrapi(iy, n) : iy;
        const bool flip = hash3(hx, hy, p.seed * 17u + 5) & 1;
        const double gx = flip ? 1 - fx : fx;
        const double d1 = std::abs(std::hypot(gx, fy) - 0.5), d2 = std::abs(std::hypot(1 - gx, 1 - fy) - 0.5);
        const double hw = wd * 0.25;
        return smooth(hw - aa, hw + aa, std::min(d1, d2));
    }
    }
    return 0;
}

Field plasma(const Params &p, int w, int h) {
    int N = 1;
    while (N < std::max(w, h) && N < 1024) N <<= 1;
    QVector<float> g(N * N);
    std::mt19937 rng(p.seed * 2654435761u + 77);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    int step = N;
    const int start = std::max(1, int(std::lround(p.scale)));
    while (step > 1 && N / step < start) step >>= 1;
    for (int y = 0; y < N; y += step) for (int x = 0; x < N; x += step) g[y * N + x] = uni(rng);
    auto at = [&](int x, int y) -> float & { return g[wrapi(y, N) * N + wrapi(x, N)]; };
    float amp = 1.f;
    const float rough = float(std::clamp(p.persistence, 0.05, 0.95));
    while (step > 1) {
        const int hs = step / 2;
        amp *= rough * 1.6f;
        for (int y = 0; y < N; y += step) for (int x = 0; x < N; x += step)   // diamond
            at(x + hs, y + hs) = (at(x, y) + at(x + step, y) + at(x, y + step) + at(x + step, y + step)) * 0.25f + uni(rng) * amp;
        for (int y = 0; y < N; y += hs) for (int x = (y / hs % 2 == 0) ? hs : 0; x < N; x += step)   // square
            at(x, y) = (at(x - hs, y) + at(x + hs, y) + at(x, y - hs) + at(x, y + hs)) * 0.25f + uni(rng) * amp;
        step = hs;
    }
    Field f; f.w = w; f.h = h; f.v.resize(w * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {   // bilinear, wrapping
        const double sx = (x + 0.5) * N / w - 0.5, sy = (y + 0.5) * N / h - 0.5;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const float ax = float(sx - x0), ay = float(sy - y0);
        f.v[y * w + x] = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
    }
    normalise(f.v);
    return f;
}

// sample a cell grid (gw x gh) up to w x h by nearest cell
Field fromCells(const QVector<uchar> &cells, int gw, int gh, int w, int h, int nStates) {
    Field f; f.w = w; f.h = h; f.v.resize(w * h); f.st.resize(w * h); f.nStates = nStates;
    for (int y = 0; y < h; ++y) {
        const int cy = std::min(gh - 1, y * gh / h);
        for (int x = 0; x < w; ++x) {
            const int cx = std::min(gw - 1, x * gw / w);
            const uchar s = cells[cy * gw + cx];
            f.st[y * w + x] = s;
            f.v[y * w + x] = nStates > 1 ? float(s) / (nStates - 1) : float(s);
        }
    }
    return f;
}

QVector<uchar> randomCells(int n, int states, double density, quint32 seed) {
    std::mt19937 rng(seed * 2246822519u + 13);
    std::uniform_real_distribution<double> uni(0, 1);
    QVector<uchar> g(n);
    for (uchar &c : g) {
        if (states <= 2) c = uni(rng) < density ? 1 : 0;
        else c = uchar(std::min(states - 1, int(uni(rng) * states)));
    }
    return g;
}

Field grayScott(const Params &p, int w, int h, const std::atomic<bool> *cancel) {
    const int cs = std::max(1, p.cell);
    const int gw = std::clamp(w / cs, 8, 320), gh = std::clamp(h / cs, 8, 320);
    QVector<float> U(gw * gh, 1.f), V(gw * gh, 0.f), U2(gw * gh), V2(gw * gh);
    std::mt19937 rng(p.seed * 374761393u + 7);
    std::uniform_int_distribution<int> rx(0, gw - 1), ry(0, gh - 1);
    const int blobs = std::max(6, gw * gh / 110);
    for (int b = 0; b < blobs; ++b) {
        const int bx = rx(rng), by = ry(rng);
        for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {
            const int k = wrapi(by + j, gh) * gw + wrapi(bx + i, gw);
            U[k] = 0.5f; V[k] = 0.25f;
        }
    }
    const float F = float(p.feed), K = float(p.kill), Du = 1.0f, Dv = 0.5f;
    const int steps = std::clamp(p.generations, 1, 20000);
    for (int s = 0; s < steps; ++s) {
        if (cancel && cancel->load()) break;
        for (int y = 0; y < gh; ++y) {
            const int ym = wrapi(y - 1, gh) * gw, yp = wrapi(y + 1, gh) * gw, yc = y * gw;
            for (int x = 0; x < gw; ++x) {
                const int xm = x == 0 ? gw - 1 : x - 1, xp = x == gw - 1 ? 0 : x + 1;
                const int k = yc + x;
                const float lu = 0.2f * (U[yc + xm] + U[yc + xp] + U[ym + x] + U[yp + x]) + 0.05f * (U[ym + xm] + U[ym + xp] + U[yp + xm] + U[yp + xp]) - U[k];
                const float lv = 0.2f * (V[yc + xm] + V[yc + xp] + V[ym + x] + V[yp + x]) + 0.05f * (V[ym + xm] + V[ym + xp] + V[yp + xm] + V[yp + xp]) - V[k];
                const float uvv = U[k] * V[k] * V[k];
                U2[k] = std::clamp(U[k] + Du * lu - uvv + F * (1 - U[k]), 0.f, 1.f);
                V2[k] = std::clamp(V[k] + Dv * lv + uvv - (F + K) * V[k], 0.f, 1.f);
            }
        }
        U.swap(U2); V.swap(V2);
    }
    Field f; f.w = w; f.h = h; f.v.resize(w * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const double sx = (x + 0.5) * gw / w - 0.5, sy = (y + 0.5) * gh / h - 0.5;
        const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
        const float ax = float(sx - x0), ay = float(sy - y0);
        auto at = [&](int xx, int yy) { return V[wrapi(yy, gh) * gw + wrapi(xx, gw)]; };
        f.v[y * w + x] = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
    }
    normalise(f.v);
    return f;
}

struct Preset { const char *gen, *name; void (*apply)(Params &); };
const QVector<Preset> &presets() {
    static const QVector<Preset> list = {
        {"grayscott", "Spots", [](Params &p) { p.feed = 0.035; p.kill = 0.065; p.generations = std::max(p.generations, 2500); }},
        {"grayscott", "Stripes", [](Params &p) { p.feed = 0.022; p.kill = 0.051; p.generations = std::max(p.generations, 2500); }},
        {"grayscott", "Coral", [](Params &p) { p.feed = 0.0545; p.kill = 0.062; p.generations = std::max(p.generations, 2500); }},
        {"grayscott", "Mitosis", [](Params &p) { p.feed = 0.0367; p.kill = 0.0649; p.generations = std::max(p.generations, 2500); }},
        {"elementary", "Rule 30", [](Params &p) { p.rule = 30; }},
        {"elementary", "Rule 90", [](Params &p) { p.rule = 90; }},
        {"elementary", "Rule 110", [](Params &p) { p.rule = 110; }},
        {"elementary", "Rule 150", [](Params &p) { p.rule = 150; }},
        {"elementary", "Rule 184", [](Params &p) { p.rule = 184; }},
        {"life", "Conway", [](Params &p) { p.lifeRule = QStringLiteral("B3/S23"); }},
        {"life", "HighLife", [](Params &p) { p.lifeRule = QStringLiteral("B36/S23"); }},
        {"life", "Seeds", [](Params &p) { p.lifeRule = QStringLiteral("B2/S"); p.generations = 6; p.density = 0.05; }},
        {"life", "Day & Night", [](Params &p) { p.lifeRule = QStringLiteral("B3678/S34678"); p.density = 0.5; }},
        {"life", "Maze", [](Params &p) { p.lifeRule = QStringLiteral("B3/S12345"); p.density = 0.08; p.generations = 200; }},
        {"brain", "Classic", [](Params &p) { p.density = 0.3; p.generations = 60; }},
        {"brain", "Sparse Gliders", [](Params &p) { p.density = 0.08; p.generations = 120; }},
        {"brain", "Dense Storm", [](Params &p) { p.density = 0.55; p.generations = 30; }},
        {"cyclic", "Classic CCA", [](Params &p) { p.states = 14; p.threshold = 1; p.range = 1; p.moore = false; p.generations = 300; }},
        {"cyclic", "Demons", [](Params &p) { p.states = 12; p.threshold = 1; p.range = 1; p.moore = true; p.generations = 300; }},
        {"cyclic", "Spirals", [](Params &p) { p.states = 8; p.threshold = 5; p.range = 3; p.moore = true; p.generations = 150; }},
        {"cyclic", "313", [](Params &p) { p.states = 3; p.threshold = 3; p.range = 3; p.moore = true; p.generations = 120; }},
        {"worley", "Cells", [](Params &p) { p.variant = 5; }},
        {"worley", "Stones", [](Params &p) { p.variant = 4; }},
        {"worley", "Crackle", [](Params &p) { p.variant = 3; }},
    };
    return list;
}
} // namespace

// ------------------------------------------------------------------ noise primitives
double perlin(double x, double y, int px, int py, quint32 seed) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    auto grad = [&](int ix, int iy, double dx, double dy) {
        if (px > 0) ix = wrapi(ix, px);
        if (py > 0) iy = wrapi(iy, py);
        const double a = (hash3(ix, iy, seed) & 0xFFFF) / 65536.0 * 2 * kPi;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const double n00 = grad(x0, y0, fx, fy), n10 = grad(x0 + 1, y0, fx - 1, fy);
    const double n01 = grad(x0, y0 + 1, fx, fy - 1), n11 = grad(x0 + 1, y0 + 1, fx - 1, fy - 1);
    const double u = fade(fx), v = fade(fy);
    return ((n00 + (n10 - n00) * u) + ((n01 + (n11 - n01) * u) - (n00 + (n10 - n00) * u)) * v) * 1.41421356;
}

double simplex(double xin, double yin, quint32 seed) {
    const double F2 = 0.5 * (std::sqrt(3.0) - 1.0), G2 = (3.0 - std::sqrt(3.0)) / 6.0;
    const double s = (xin + yin) * F2;
    const int i = int(std::floor(xin + s)), j = int(std::floor(yin + s));
    const double t = (i + j) * G2;
    const double x0 = xin - (i - t), y0 = yin - (j - t);
    const int i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
    const double x1 = x0 - i1 + G2, y1 = y0 - j1 + G2, x2 = x0 - 1 + 2 * G2, y2 = y0 - 1 + 2 * G2;
    auto corner = [&](int gi, int gj, double dx, double dy) {
        double tt = 0.5 - dx * dx - dy * dy;
        if (tt < 0) return 0.0;
        const double a = (hash3(gi, gj, seed ^ 0xA5A5u) & 0xFFFF) / 65536.0 * 2 * kPi;
        tt *= tt;
        return tt * tt * (std::cos(a) * dx + std::sin(a) * dy);
    };
    return 70.0 * (corner(i, j, x0, y0) + corner(i + i1, j + j1, x1, y1) + corner(i + 1, j + 1, x2, y2));
}

// ------------------------------------------------------------------ cellular automata
QVector<QVector<uchar>> elementary(int rule, int width, int rows_, bool singleCell, quint32 seed, double density) {
    QVector<QVector<uchar>> out;
    if (width <= 0 || rows_ <= 0) return out;
    QVector<uchar> row(width, 0);
    if (singleCell) row[width / 2] = 1;
    else row = randomCells(width, 2, density, seed);
    out.append(row);
    for (int r = 1; r < rows_; ++r) {
        QVector<uchar> next(width);
        for (int x = 0; x < width; ++x) {
            const int l = row[wrapi(x - 1, width)], c = row[x], rr = row[wrapi(x + 1, width)];
            next[x] = uchar((rule >> ((l << 2) | (c << 1) | rr)) & 1);
        }
        row = next;
        out.append(row);
    }
    return out;
}

LifeRule parseLifeRule(const QString &s) {
    LifeRule r;
    static const QRegularExpression re(QStringLiteral("^\\s*[Bb]([0-8]*)\\s*/\\s*[Ss]([0-8]*)\\s*$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return r;
    for (QChar c : m.captured(1)) r.born |= quint16(1u << c.digitValue());
    for (QChar c : m.captured(2)) r.survive |= quint16(1u << c.digitValue());
    r.valid = true;
    return r;
}

static inline int moore8(const QVector<uchar> &g, int w, int h, int x, int y, uchar state) {
    int n = 0;
    for (int j = -1; j <= 1; ++j) {
        const int yy = wrapi(y + j, h) * w;
        for (int i = -1; i <= 1; ++i) if ((i || j) && g[yy + wrapi(x + i, w)] == state) ++n;
    }
    return n;
}

QVector<uchar> lifeStep(const QVector<uchar> &g, int w, int h, const LifeRule &r) {
    QVector<uchar> out(w * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int n = moore8(g, w, h, x, y, 1);
        const bool alive = g[y * w + x];
        out[y * w + x] = alive ? ((r.survive >> n) & 1) : ((r.born >> n) & 1);
    }
    return out;
}

QVector<uchar> briansBrainStep(const QVector<uchar> &g, int w, int h) {
    QVector<uchar> out(w * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const uchar s = g[y * w + x];
        out[y * w + x] = s == 1 ? 2 : s == 2 ? 0 : (moore8(g, w, h, x, y, 1) == 2 ? 1 : 0);
    }
    return out;
}

QVector<uchar> cyclicStep(const QVector<uchar> &g, int w, int h, int n, int threshold, int range, bool moore) {
    QVector<uchar> out(w * h);
    n = std::max(2, n);
    range = std::clamp(range, 1, 10);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const uchar s = g[y * w + x], nx = uchar((s + 1) % n);
        int c = 0;
        for (int j = -range; j <= range && c < threshold; ++j) {
            const int yy = wrapi(y + j, h) * w;
            for (int i = -range; i <= range; ++i) {
                if (!i && !j) continue;
                if (!moore && std::abs(i) + std::abs(j) > range) continue;
                if (g[yy + wrapi(x + i, w)] == nx && ++c >= threshold) break;
            }
        }
        out[y * w + x] = c >= threshold ? nx : s;
    }
    return out;
}

// ------------------------------------------------------------------ catalogue
QStringList generatorKeys() {
    return {QStringLiteral("perlin"), QStringLiteral("simplex"), QStringLiteral("fbm"), QStringLiteral("turbulence"),
            QStringLiteral("ridged"), QStringLiteral("warp"), QStringLiteral("marble"), QStringLiteral("wood"),
            QStringLiteral("clouds"), QStringLiteral("plasma"), QStringLiteral("worley"), QStringLiteral("grayscott"),
            QStringLiteral("elementary"), QStringLiteral("life"), QStringLiteral("brain"), QStringLiteral("cyclic"),
            QStringLiteral("pattern")};
}

QString generatorLabel(const QString &k) {
    static const QHash<QString, QString> l = {
        {QStringLiteral("perlin"), QStringLiteral("Perlin Noise")}, {QStringLiteral("simplex"), QStringLiteral("Simplex Noise")},
        {QStringLiteral("fbm"), QStringLiteral("fBm")}, {QStringLiteral("turbulence"), QStringLiteral("Turbulence")},
        {QStringLiteral("ridged"), QStringLiteral("Ridged Multifractal")}, {QStringLiteral("warp"), QStringLiteral("Domain-Warped Noise")},
        {QStringLiteral("marble"), QStringLiteral("Marble")}, {QStringLiteral("wood"), QStringLiteral("Wood Grain")},
        {QStringLiteral("clouds"), QStringLiteral("Clouds")}, {QStringLiteral("plasma"), QStringLiteral("Plasma")},
        {QStringLiteral("worley"), QStringLiteral("Worley / Voronoi")}, {QStringLiteral("grayscott"), QStringLiteral("Reaction-Diffusion (Gray-Scott)")},
        {QStringLiteral("elementary"), QStringLiteral("Elementary CA (Wolfram)")}, {QStringLiteral("life"), QStringLiteral("Life-like CA")},
        {QStringLiteral("brain"), QStringLiteral("Brian's Brain")}, {QStringLiteral("cyclic"), QStringLiteral("Cyclic CA")},
        {QStringLiteral("pattern"), QStringLiteral("Pattern")}};
    return l.value(k, k);
}

QStringList presetNames(const QString &gen) {
    QStringList out;
    for (const Preset &p : presets()) if (gen == QLatin1String(p.gen)) out << QString::fromUtf8(p.name);
    if (gen == QLatin1String("pattern")) out = patternNames();
    return out;
}

bool applyPreset(Params &p, const QString &name) {
    if (p.gen == QLatin1String("pattern")) {
        const int i = patternNames().indexOf(name);
        if (i < 0) return false;
        p.variant = i;
        return true;
    }
    for (const Preset &pr : presets())
        if (p.gen == QLatin1String(pr.gen) && name == QString::fromUtf8(pr.name)) { pr.apply(p); return true; }
    return false;
}

QStringList worleyVariants() {
    return {QStringLiteral("F1"), QStringLiteral("F2"), QStringLiteral("F2 - F1"), QStringLiteral("Crackle"), QStringLiteral("Stones"), QStringLiteral("Cells")};
}
QStringList patternNames() {
    return {QStringLiteral("Checker"), QStringLiteral("Grid"), QStringLiteral("Stripes"), QStringLiteral("Dots"), QStringLiteral("Brick"),
            QStringLiteral("Hex"), QStringLiteral("Halftone"), QStringLiteral("Truchet")};
}
QStringList lifePresets() { return presetNames(QStringLiteral("life")); }
QString lifePresetRule(const QString &name) { Params p; p.gen = QStringLiteral("life"); applyPreset(p, name); return p.lifeRule; }

QStringList usedParams(const QString &g) {
    if (g == QLatin1String("perlin") || g == QLatin1String("simplex") || g == QLatin1String("fbm") || g == QLatin1String("turbulence") || g == QLatin1String("ridged") || g == QLatin1String("clouds"))
        return {QStringLiteral("noise"), QStringLiteral("octaves")};
    if (g == QLatin1String("warp") || g == QLatin1String("marble") || g == QLatin1String("wood")) return {QStringLiteral("noise"), QStringLiteral("octaves"), QStringLiteral("warp")};
    if (g == QLatin1String("plasma")) return {QStringLiteral("octaves")};
    if (g == QLatin1String("worley")) return {QStringLiteral("worley")};
    if (g == QLatin1String("pattern")) return {QStringLiteral("pattern")};
    if (g == QLatin1String("grayscott")) return {QStringLiteral("rd"), QStringLiteral("cell"), QStringLiteral("generations")};
    if (g == QLatin1String("elementary")) return {QStringLiteral("rule"), QStringLiteral("cell"), QStringLiteral("density")};
    if (g == QLatin1String("life")) return {QStringLiteral("life"), QStringLiteral("cell"), QStringLiteral("generations"), QStringLiteral("density")};
    if (g == QLatin1String("brain")) return {QStringLiteral("states"), QStringLiteral("cell"), QStringLiteral("generations"), QStringLiteral("density")};
    if (g == QLatin1String("cyclic")) return {QStringLiteral("cyclic"), QStringLiteral("cell"), QStringLiteral("generations")};
    return {};
}

bool Params::operator==(const Params &o) const { return toString(*this) == toString(o); }

QJsonObject toJson(const Params &p) {
    QJsonObject j;
    j[QStringLiteral("gen")] = p.gen; j[QStringLiteral("seed")] = double(p.seed);
    j[QStringLiteral("w")] = p.width; j[QStringLiteral("h")] = p.height; j[QStringLiteral("tile")] = p.tileable;
    j[QStringLiteral("scale")] = p.scale; j[QStringLiteral("contrast")] = p.contrast; j[QStringLiteral("bright")] = p.brightness;
    j[QStringLiteral("invert")] = p.invert; j[QStringLiteral("basis")] = p.basis; j[QStringLiteral("oct")] = p.octaves;
    j[QStringLiteral("pers")] = p.persistence; j[QStringLiteral("lac")] = p.lacunarity; j[QStringLiteral("warp")] = p.warp;
    j[QStringLiteral("variant")] = p.variant; j[QStringLiteral("metric")] = p.metric; j[QStringLiteral("width")] = p.width2;
    j[QStringLiteral("cell")] = p.cell; j[QStringLiteral("gens")] = p.generations; j[QStringLiteral("density")] = p.density;
    j[QStringLiteral("rule")] = p.rule; j[QStringLiteral("single")] = p.singleCell; j[QStringLiteral("life")] = p.lifeRule;
    j[QStringLiteral("states")] = p.states; j[QStringLiteral("thresh")] = p.threshold; j[QStringLiteral("range")] = p.range;
    j[QStringLiteral("moore")] = p.moore; j[QStringLiteral("feed")] = p.feed; j[QStringLiteral("kill")] = p.kill;
    QJsonArray ramp;
    for (const GradStop &s : p.ramp) ramp.append(QJsonArray{s.pos, s.color.name(QColor::HexArgb)});
    if (!ramp.isEmpty()) j[QStringLiteral("ramp")] = ramp;
    QJsonArray sc;
    for (const QColor &c : p.stateColors) sc.append(c.name(QColor::HexArgb));
    if (!sc.isEmpty()) j[QStringLiteral("stateColors")] = sc;
    return j;
}

Params fromJson(const QJsonObject &j) {
    Params p, d;
    p.gen = j.value(QStringLiteral("gen")).toString(d.gen);
    if (!generatorKeys().contains(p.gen)) p.gen = d.gen;
    p.seed = quint32(j.value(QStringLiteral("seed")).toDouble(d.seed));
    p.width = std::clamp(j.value(QStringLiteral("w")).toInt(d.width), 4, 4096);
    p.height = std::clamp(j.value(QStringLiteral("h")).toInt(d.height), 4, 4096);
    p.tileable = j.value(QStringLiteral("tile")).toBool(d.tileable);
    p.scale = std::clamp(j.value(QStringLiteral("scale")).toDouble(d.scale), 0.5, 256.0);
    p.contrast = std::clamp(j.value(QStringLiteral("contrast")).toDouble(d.contrast), -100.0, 100.0);
    p.brightness = std::clamp(j.value(QStringLiteral("bright")).toDouble(d.brightness), -100.0, 100.0);
    p.invert = j.value(QStringLiteral("invert")).toBool(false);
    p.basis = std::clamp(j.value(QStringLiteral("basis")).toInt(d.basis), 0, 1);
    p.octaves = std::clamp(j.value(QStringLiteral("oct")).toInt(d.octaves), 1, 12);
    p.persistence = std::clamp(j.value(QStringLiteral("pers")).toDouble(d.persistence), 0.0, 1.0);
    p.lacunarity = std::clamp(j.value(QStringLiteral("lac")).toDouble(d.lacunarity), 1.0, 4.0);
    p.warp = std::clamp(j.value(QStringLiteral("warp")).toDouble(d.warp), 0.0, 10.0);
    p.variant = std::clamp(j.value(QStringLiteral("variant")).toInt(d.variant), 0, 7);
    p.metric = std::clamp(j.value(QStringLiteral("metric")).toInt(d.metric), 0, 2);
    p.width2 = std::clamp(j.value(QStringLiteral("width")).toDouble(d.width2), 0.0, 1.0);
    p.cell = std::clamp(j.value(QStringLiteral("cell")).toInt(d.cell), 1, 64);
    p.generations = std::clamp(j.value(QStringLiteral("gens")).toInt(d.generations), 0, 20000);
    p.density = std::clamp(j.value(QStringLiteral("density")).toDouble(d.density), 0.0, 1.0);
    p.rule = std::clamp(j.value(QStringLiteral("rule")).toInt(d.rule), 0, 255);
    p.singleCell = j.value(QStringLiteral("single")).toBool(d.singleCell);
    p.lifeRule = j.value(QStringLiteral("life")).toString(d.lifeRule);
    p.states = std::clamp(j.value(QStringLiteral("states")).toInt(d.states), 2, 64);
    p.threshold = std::clamp(j.value(QStringLiteral("thresh")).toInt(d.threshold), 1, 100);
    p.range = std::clamp(j.value(QStringLiteral("range")).toInt(d.range), 1, 10);
    p.moore = j.value(QStringLiteral("moore")).toBool(d.moore);
    p.feed = std::clamp(j.value(QStringLiteral("feed")).toDouble(d.feed), 0.0, 0.12);
    p.kill = std::clamp(j.value(QStringLiteral("kill")).toDouble(d.kill), 0.0, 0.12);
    for (const QJsonValue &v : j.value(QStringLiteral("ramp")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 2) p.ramp.append({std::clamp(a[0].toDouble(), 0.0, 1.0), QColor(a[1].toString())});
    }
    for (const QJsonValue &v : j.value(QStringLiteral("stateColors")).toArray()) p.stateColors.append(QColor(v.toString()));
    return p;
}

QString toString(const Params &p) { return QString::fromUtf8(QJsonDocument(toJson(p)).toJson(QJsonDocument::Compact)); }
Params fromString(const QString &s) { return fromJson(QJsonDocument::fromJson(s.toUtf8()).object()); }

// ------------------------------------------------------------------ generation
Field generate(const Params &p, int w, int h, const std::atomic<bool> *cancel) {
    w = std::max(1, w); h = std::max(1, h);
    const QString &g = p.gen;
    Field f;
    const int cs = std::max(1, p.cell);
    const int gw = std::max(1, (w + cs - 1) / cs), gh = std::max(1, (h + cs - 1) / cs);
    if (g == QLatin1String("perlin")) { f = pointwise(p, w, h, cancel, genPerlin); normalise(f.v); }
    else if (g == QLatin1String("simplex")) { f = pointwise(p, w, h, cancel, genSimplex); normalise(f.v); }
    else if (g == QLatin1String("fbm")) { f = pointwise(p, w, h, cancel, genFbm); normalise(f.v); }
    else if (g == QLatin1String("turbulence")) { f = pointwise(p, w, h, cancel, genTurb); normalise(f.v); }
    else if (g == QLatin1String("ridged")) { f = pointwise(p, w, h, cancel, genRidged); normalise(f.v); }
    else if (g == QLatin1String("warp")) { f = pointwise(p, w, h, cancel, genWarp); normalise(f.v); }
    else if (g == QLatin1String("marble")) f = pointwise(p, w, h, cancel, genMarble);
    else if (g == QLatin1String("wood")) f = pointwise(p, w, h, cancel, genWood);
    else if (g == QLatin1String("clouds")) { f = pointwise(p, w, h, cancel, genFbm); normalise(f.v); for (float &v : f.v) v = float(smooth(0.2, 0.9, v)); }
    else if (g == QLatin1String("plasma")) f = plasma(p, w, h);
    else if (g == QLatin1String("worley")) { f = pointwise(p, w, h, cancel, genWorley); if (p.variant <= 2) normalise(f.v); }
    else if (g == QLatin1String("pattern")) f = pointwise(p, w, h, cancel, genPattern);
    else if (g == QLatin1String("grayscott")) f = grayScott(p, w, h, cancel);
    else if (g == QLatin1String("elementary")) {
        const auto rws = elementary(p.rule, gw, gh, p.singleCell, p.seed, p.density);
        QVector<uchar> cells; cells.reserve(gw * gh);
        for (const auto &r : rws) cells += r;
        f = fromCells(cells, gw, gh, w, h, 2);
    } else if (g == QLatin1String("life")) {
        LifeRule r = parseLifeRule(p.lifeRule);
        if (!r.valid) r = parseLifeRule(QStringLiteral("B3/S23"));
        QVector<uchar> c = randomCells(gw * gh, 2, p.density, p.seed);
        for (int i = 0; i < p.generations && !(cancel && cancel->load()); ++i) c = lifeStep(c, gw, gh, r);
        f = fromCells(c, gw, gh, w, h, 2);
    } else if (g == QLatin1String("brain")) {
        QVector<uchar> c = randomCells(gw * gh, 2, p.density, p.seed);
        for (int i = 0; i < p.generations && !(cancel && cancel->load()); ++i) c = briansBrainStep(c, gw, gh);
        f = fromCells(c, gw, gh, w, h, 3);
    } else if (g == QLatin1String("cyclic")) {
        const int n = std::max(2, p.states);
        QVector<uchar> c = randomCells(gw * gh, n, 0, p.seed);
        for (int i = 0; i < p.generations && !(cancel && cancel->load()); ++i) c = cyclicStep(c, gw, gh, n, p.threshold, p.range, p.moore);
        f = fromCells(c, gw, gh, w, h, n);
    } else f = pointwise(p, w, h, cancel, genFbm);
    // contrast / brightness / invert
    const double c = p.contrast;
    const double k = c >= 0 ? 100.0 / std::max(1.0, 100.0 - c) : (100.0 + c) / 100.0;
    const double b = p.brightness / 100.0;
    if (c != 0 || b != 0 || p.invert)
        for (float &v : f.v) {
            double x = (v - 0.5) * k + 0.5 + b;
            if (p.invert) x = 1 - x;
            v = float(std::clamp(x, 0.0, 1.0));
        }
    return f;
}

QImage render(const Params &p, int w, int h, const std::atomic<bool> *cancel) {
    if (w <= 0) w = p.width;
    if (h <= 0) h = p.height;
    const Field f = generate(p, w, h, cancel);
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    // per-state colours (Brian's Brain default palette), else the ramp
    const bool perState = !f.st.isEmpty() && (!p.stateColors.isEmpty() || p.gen == QLatin1String("brain")) && p.contrast == 0 && p.brightness == 0 && !p.invert;
    QVector<QRgb> stateLut;
    if (perState) {
        QVector<QColor> cols = p.stateColors;
        if (cols.isEmpty()) cols = {QColor(10, 10, 30), QColor(255, 255, 255), QColor(60, 110, 255)};
        for (int i = 0; i < f.nStates; ++i) {
            const QColor c = i < cols.size() ? cols[i] : fills::sampleGradient(p.ramp.isEmpty() ? QVector<GradStop>{{0, Qt::black}, {1, Qt::white}} : p.ramp, f.nStates > 1 ? double(i) / (f.nStates - 1) : 0);
            stateLut << qPremultiply(c.rgba());
        }
    }
    QRgb lut[1024];
    const QVector<GradStop> ramp = p.ramp.isEmpty() ? QVector<GradStop>{{0, Qt::black}, {1, Qt::white}} : p.ramp;
    for (int i = 0; i < 1024; ++i) lut[i] = qPremultiply(fills::sampleGradient(ramp, i / 1023.0).rgba());
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int k = y * w + x;
            line[x] = perState ? stateLut.value(f.st[k], 0) : lut[std::clamp(int(f.v[k] * 1023 + 0.5f), 0, 1023)];
        }
    }
    return img;
}

QImage renderGray(const Params &p, int w, int h) {
    return render(p, w, h).convertToFormat(QImage::Format_Grayscale8);
}

static QImage cachedTile(const QString &json, bool gray) {
    static QMutex mu;
    static QHash<QString, QImage> cache;
    const QString key = (gray ? QLatin1String("g:") : QLatin1String("c:")) + json;
    {
        QMutexLocker l(&mu);
        auto it = cache.constFind(key);
        if (it != cache.constEnd()) return *it;
    }
    const Params p = fromString(json);
    const QImage img = gray ? renderGray(p) : render(p);
    QMutexLocker l(&mu);
    if (cache.size() > 64) cache.clear();
    cache.insert(key, img);
    return img;
}
QImage patternTile(const QString &json) { return cachedTile(json, false); }
QImage textureTile(const QString &json) { return cachedTile(json, true); }

QImage contactSheet(int tile, int columns) {
    auto ramp = [](std::initializer_list<std::pair<double, const char *>> l) {
        QVector<GradStop> r;
        for (const auto &e : l) r.append(GradStop{e.first, QColor(QString::fromLatin1(e.second))});
        return r;
    };
    const QVector<GradStop> fire = ramp({{0, "#100008"}, {0.4, "#a01010"}, {0.75, "#ffa020"}, {1, "#fff8c0"}});
    const QVector<GradStop> sea = ramp({{0, "#031a33"}, {0.5, "#1c6fa8"}, {1, "#e8f6ff"}});
    const QVector<GradStop> marble = ramp({{0, "#2a2a30"}, {0.5, "#d8d4cc"}, {1, "#ffffff"}});
    const QVector<GradStop> wood = ramp({{0, "#5a3416"}, {0.6, "#a8703a"}, {1, "#d9a564"}});
    const QVector<GradStop> sky = ramp({{0, "#3a78c8"}, {0.55, "#9cc4ee"}, {1, "#ffffff"}});
    const QVector<GradStop> moss = ramp({{0, "#0d1f0a"}, {0.5, "#4f8a2b"}, {1, "#e4f2a0"}});
    const QVector<GradStop> neon = ramp({{0, "#1a0033"}, {0.33, "#ff2fa0"}, {0.66, "#2fd0ff"}, {1, "#fff35c"}});
    struct Entry { QString label; Params p; };
    QVector<Entry> entries;
    auto add = [&](const QString &gen, const QString &preset, const QVector<GradStop> &r, std::function<void(Params &)> tweak = nullptr) {
        Params p; p.gen = gen; p.seed = 7; p.width = p.height = tile; p.ramp = r;
        if (!preset.isEmpty()) applyPreset(p, preset);
        if (tweak) tweak(p);
        entries.append({preset.isEmpty() ? generatorLabel(gen) : generatorLabel(gen).section(QLatin1Char(' '), 0, 0) + QStringLiteral(": ") + preset, p});
    };
    add(QStringLiteral("perlin"), {}, {});
    add(QStringLiteral("simplex"), {}, {});
    add(QStringLiteral("fbm"), {}, sea);
    add(QStringLiteral("turbulence"), {}, fire);
    add(QStringLiteral("ridged"), {}, moss);
    add(QStringLiteral("warp"), {}, neon);
    add(QStringLiteral("marble"), {}, marble);
    add(QStringLiteral("wood"), {}, wood);
    add(QStringLiteral("clouds"), {}, sky);
    add(QStringLiteral("plasma"), {}, neon);
    for (const QString &v : worleyVariants()) {
        const int i = worleyVariants().indexOf(v);
        entries.append({QStringLiteral("Worley: ") + v, [&] { Params p; p.gen = QStringLiteral("worley"); p.seed = 7; p.width = p.height = tile; p.variant = i; p.scale = 6; if (i >= 3) p.ramp = i == 4 ? marble : sea; return p; }()});
    }
    for (const QString &n : presetNames(QStringLiteral("grayscott"))) add(QStringLiteral("grayscott"), n, sea, [](Params &p) { p.cell = 1; p.generations = 3000; });
    for (const QString &n : presetNames(QStringLiteral("elementary"))) add(QStringLiteral("elementary"), n, {}, [](Params &p) { p.cell = 2; });
    for (const QString &n : presetNames(QStringLiteral("life"))) add(QStringLiteral("life"), n, moss);
    add(QStringLiteral("brain"), {}, {});
    for (const QString &n : presetNames(QStringLiteral("cyclic"))) add(QStringLiteral("cyclic"), n, neon, [](Params &p) { p.cell = 1; p.generations = 400; });
    for (const QString &n : patternNames()) add(QStringLiteral("pattern"), n, {});

    const int label = 18, gap = 8;
    const int rows = (entries.size() + columns - 1) / columns;
    QImage sheet(gap + columns * (tile + gap), gap + rows * (tile + label + gap), QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor(0xd4, 0xd0, 0xc8));
    QVector<QImage> imgs(entries.size());
    // sequential: render() parallelises internally (nested blockingMap can starve the pool)
    for (int i = 0; i < entries.size(); ++i) imgs[i] = render(entries[i].p);
    QPainter pt(&sheet);
    QFont f = pt.font(); f.setPixelSize(11); pt.setFont(f);
    for (int i = 0; i < entries.size(); ++i) {
        const int x = gap + (i % columns) * (tile + gap), y = gap + (i / columns) * (tile + label + gap);
        pt.drawImage(x, y, imgs[i]);
        pt.setPen(QColor(0x80, 0x80, 0x80)); pt.drawRect(x - 1, y - 1, tile + 1, tile + 1);
        pt.setPen(Qt::black);
        pt.drawText(QRect(x, y + tile + 2, tile, label - 2), Qt::AlignHCenter | Qt::AlignTop, pt.fontMetrics().elidedText(entries[i].label, Qt::ElideRight, tile));
    }
    return sheet;
}
}
