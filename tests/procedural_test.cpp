// Procedural textures: determinism, seamless tiling, cellular automata rules, persistence.
#include "core/Document.h"
#include "core/Procedural.h"
#include "core/Render.h"
#include "io/LwdFormat.h"
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>

using namespace proc;

namespace {
double lum(QRgb c) { return 0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c); }
// mean |difference| across the wrap seam, and the largest mean difference between any two
// neighbouring inner columns (rows): a seamless tile's wrap looks like any other column pair
void seamStats(const QImage &img, bool horizontal, double *seam, double *inner) {
    const int w = img.width(), h = img.height();
    auto at = [&](int i, int j) { return horizontal ? lum(img.pixel(i, j)) : lum(img.pixel(j, i)); };
    const int n = horizontal ? w : h, m = horizontal ? h : w;
    auto pairDiff = [&](int a, int b) { double s = 0; for (int j = 0; j < m; ++j) s += std::abs(at(a, j) - at(b, j)); return s / m; };
    *seam = pairDiff(n - 1, 0);
    *inner = 0;
    for (int i = 0; i + 1 < n; ++i) *inner = std::max(*inner, pairDiff(i, i + 1));
}
}

class ProceduralTest : public QObject {
    Q_OBJECT
private slots:
    void seedDeterminism() {
        for (const QString &g : generatorKeys()) {
            Params p; p.gen = g; p.width = p.height = 64; p.seed = 42; p.generations = std::min(p.generations, 40);
            if (g == QLatin1String("pattern")) p.variant = 7;   // Truchet is the seeded pattern
            const QImage a = proc::render(p), b = proc::render(p);
            QVERIFY2(a == b, qPrintable(g));
            p.seed = 43;
            const QImage c = proc::render(p);
            QVERIFY2(a != c, qPrintable(g + QStringLiteral(" ignores the seed")));
        }
    }
    void tileableSeams() {
        const QStringList gens = {"perlin", "simplex", "fbm", "turbulence", "ridged", "warp", "marble", "wood", "clouds",
                                  "plasma", "worley", "grayscott", "life", "brain", "cyclic", "pattern"};
        for (const QString &g : gens) {
            Params p; p.gen = g; p.width = p.height = 96; p.seed = 7; p.tileable = true; p.scale = 4; p.cell = 1;   // 1 px cells: every column pair is a cell boundary
            if (g == QLatin1String("grayscott")) p.generations = 1500;
            if (g == QLatin1String("pattern")) p.variant = 7;
            const QImage img = proc::render(p);
            for (bool hz : {true, false}) {
                double seam, inner;
                seamStats(img, hz, &seam, &inner);
                QVERIFY2(seam <= inner * 1.25 + 1, qPrintable(QStringLiteral("%1 %2: seam %3 vs inner %4").arg(g).arg(hz ? "h" : "v").arg(seam).arg(inner)));
            }
        }
        // a non-tileable Perlin really does have a seam (the check above is meaningful)
        Params p; p.gen = QStringLiteral("perlin"); p.width = p.height = 96; p.tileable = false; p.scale = 4.5; p.octaves = 1;
        double seam, inner;
        seamStats(proc::render(p), true, &seam, &inner);
        QVERIFY2(seam > inner * 1.25 + 1, qPrintable(QStringLiteral("%1 %2").arg(seam).arg(inner)));
        // periodic Perlin primitive
        QVERIFY(qAbs(perlin(0.3, 0.7, 4, 4, 9) - perlin(4.3, 8.7, 4, 4, 9)) < 1e-12);
    }
    void rule30KnownRows() {
        const auto rows = elementary(30, 15, 8, true, 1);
        auto str = [](const QVector<uchar> &r) { QString s; for (uchar c : r) s += c ? QLatin1Char('1') : QLatin1Char('0'); return s; };
        QCOMPARE(str(rows[0]), QStringLiteral("000000010000000"));
        QCOMPARE(str(rows[1]), QStringLiteral("000000111000000"));
        QCOMPARE(str(rows[2]), QStringLiteral("000001100100000"));
        QCOMPARE(str(rows[3]), QStringLiteral("000011011110000"));
        QCOMPARE(str(rows[4]), QStringLiteral("000110010001000"));
        QCOMPARE(str(rows[5]), QStringLiteral("001101111011100"));
        // Rule 90 from one cell is Pascal's triangle mod 2 (Sierpinski)
        const auto r90 = elementary(90, 15, 4, true, 1);
        QCOMPARE(str(r90[3]), QStringLiteral("000010101010000"));
        QCOMPARE(str(r90[2]), QStringLiteral("000001000100000"));
    }
    void conwayBlinker() {
        const LifeRule r = parseLifeRule(QStringLiteral("B3/S23"));
        QVERIFY(r.valid);
        QCOMPARE(int(r.born), 1 << 3);
        QCOMPARE(int(r.survive), (1 << 2) | (1 << 3));
        QVERIFY(!parseLifeRule(QStringLiteral("nonsense")).valid);
        QVERIFY(parseLifeRule(QStringLiteral("B2/S")).valid);
        const int w = 5, h = 5;
        QVector<uchar> g(w * h, 0);
        g[2 * w + 1] = g[2 * w + 2] = g[2 * w + 3] = 1;        // horizontal
        QVector<uchar> v(w * h, 0);
        v[1 * w + 2] = v[2 * w + 2] = v[3 * w + 2] = 1;        // vertical
        const QVector<uchar> g1 = lifeStep(g, w, h, r);
        QCOMPARE(g1, v);
        QCOMPARE(lifeStep(g1, w, h, r), g);
        QCOMPARE(lifePresetRule(QStringLiteral("HighLife")), QStringLiteral("B36/S23"));
    }
    void briansBrainCycle() {
        const int w = 7, h = 7;
        QVector<uchar> g(w * h, 0);
        g[3 * w + 3] = 1;                                     // a lone firing cell
        QVector<uchar> g1 = briansBrainStep(g, w, h);
        QCOMPARE(int(g1[3 * w + 3]), 2);                      // dying (refractory)
        QVector<uchar> g2 = briansBrainStep(g1, w, h);
        QCOMPARE(int(g2[3 * w + 3]), 0);                      // off
        QVERIFY(std::all_of(g2.begin(), g2.end(), [](uchar c) { return c == 0; }));
        // B2: an off cell with exactly two firing neighbours fires
        QVector<uchar> t(w * h, 0);
        t[3 * w + 2] = t[3 * w + 4] = 1;
        const QVector<uchar> t1 = briansBrainStep(t, w, h);
        QCOMPARE(int(t1[2 * w + 3]), 1);
        QCOMPARE(int(t1[3 * w + 3]), 1);
        QCOMPARE(int(t1[3 * w + 2]), 2);
        // per-state colours
        Params p; p.gen = QStringLiteral("brain"); p.width = p.height = 32; p.generations = 3;
        p.stateColors = {Qt::black, Qt::red, Qt::blue};
        const QImage img = proc::render(p);
        QSet<QRgb> cols;
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) cols.insert(img.pixel(x, y));
        for (QRgb c : cols) QVERIFY(c == qRgb(0, 0, 0) || c == qRgb(255, 0, 0) || c == qRgb(0, 0, 255));
        QVERIFY(cols.size() == 3);
    }
    void cyclicAdvances() {
        const int w = 3, h = 1;
        QVector<uchar> g = {0, 1, 0};
        const QVector<uchar> g1 = cyclicStep(g, w, h, 3, 1, 1, false);
        QCOMPARE(int(g1[0]), 1);   // its neighbour has the successor state
        QCOMPARE(int(g1[1]), 1);   // nothing at 2 nearby: stays
    }
    void rampAndContrast() {
        Params p; p.gen = QStringLiteral("pattern"); p.variant = 0; p.scale = 2; p.width = p.height = 32;
        p.ramp = {{0, QColor(255, 0, 0)}, {1, QColor(0, 0, 255)}};
        const QImage img = proc::render(p);
        QCOMPARE(img.pixel(1, 1), qRgb(255, 0, 0));
        QCOMPARE(img.pixel(20, 1), qRgb(0, 0, 255));
        p.invert = true;
        QCOMPARE(proc::render(p).pixel(1, 1), qRgb(0, 0, 255));
        Params n; n.gen = QStringLiteral("perlin"); n.width = n.height = 64;
        n.contrast = 80;
        const QImage hi = renderGray(n);
        int extreme = 0;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) { const int v = qGray(hi.pixel(x, y)); if (v < 10 || v > 245) ++extreme; }
        QVERIFY(extreme > 64 * 64 / 5);
    }
    void presetsAndJson() {
        for (const QString &g : generatorKeys()) {
            Params p; p.gen = g;
            for (const QString &pr : presetNames(g)) QVERIFY2(applyPreset(p, pr), qPrintable(g + QLatin1Char('/') + pr));
        }
        for (const char *g : {"brain", "cyclic", "life", "grayscott", "elementary", "worley"})
            QVERIFY2(presetNames(QString::fromLatin1(g)).size() >= 3, g);
        Params p; p.gen = QStringLiteral("grayscott"); applyPreset(p, QStringLiteral("Coral"));
        QCOMPARE(p.feed, 0.0545);
        p.seed = 1234567; p.ramp = {{0, Qt::black}, {0.5, QColor(10, 200, 30)}, {1, Qt::white}}; p.stateColors = {Qt::red};
        const Params q = fromString(toString(p));
        QVERIFY(q == p);
        QCOMPARE(q.seed, quint32(1234567));
        QCOMPARE(q.ramp.size(), 3);
    }
    void fillsAndLwdKeepParameters() {
        QTemporaryDir tmp;
        std::unique_ptr<Document> d(Document::createNew(120, 80, Qt::white));
        Params tp; tp.gen = QStringLiteral("worley"); tp.variant = 3; tp.width = tp.height = 32;
        Params pp; pp.gen = QStringLiteral("pattern"); pp.variant = 0; pp.scale = 4; pp.width = pp.height = 16;
        pp.ramp = {{0, Qt::red}, {1, Qt::yellow}};
        VObject r; r.type = ObjType::Rect; r.rect = QRectF(0, 0, 120, 80); r.strokeOn = false; r.id = d->newId();
        r.fillKind = FillKind::Pattern; r.pattern = QStringLiteral("Procedural"); r.patternProc = toString(pp);
        r.texture = QStringLiteral("Procedural"); r.textureProc = toString(tp); r.textureAmount = 40;
        d->addObject(r, 0, false);
        const QImage before = d->render(0, false);
        QCOMPARE(QColor(before.pixel(1, 1)).red(), 255);   // the pattern's ramp colours show through
        const QString path = tmp.filePath(QStringLiteral("t.lwd"));
        QString err;
        QVERIFY2(lwd::save(*d, path, &err), qPrintable(err));
        std::unique_ptr<Document> back(lwd::load(path, &err));
        QVERIFY2(back, qPrintable(err));
        const VObject &o = back->objects(0).last();
        QCOMPARE(o.pattern, QStringLiteral("Procedural"));
        QVERIFY(fromString(o.patternProc) == pp);
        QVERIFY(fromString(o.textureProc) == tp);
        QCOMPARE(back->render(0, false), before);
    }
    void fastPreview() {
        QElapsedTimer t; t.start();
        Params p; p.gen = QStringLiteral("warp"); p.octaves = 5;
        proc::render(p, 256, 256);
        QVERIFY2(t.elapsed() < 4000, qPrintable(QString::number(t.elapsed())));
    }
};

QTEST_MAIN(ProceduralTest)
#include "procedural_test.moc"
