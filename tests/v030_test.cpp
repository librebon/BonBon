// v0.3.0 tests: the new filters (deterministic), Liquify, content-aware fill, C header dither,
// slices / image map export, Batch Process (core, dialog and CLI), Auto Shapes (params, handles, LWD, SVG,
// PDF, convert to paths) and the new menu wiring. Run with QT_QPA_PLATFORM=offscreen (ctest sets this).
#include "core/AutoShape.h"
#include "core/Document.h"
#include "core/Effects.h"
#include "core/FxExtra.h"
#include "core/Paths.h"
#include "core/Render.h"
#include "io/Batch.h"
#include "io/DocIO.h"
#include "io/ExportPresets.h"
#include "io/HtmlExport.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "ui/BatchDialog.h"
#include "ui/DocumentView.h"
#include "ui/LiquifyDialog.h"
#include "ui/MainWindow.h"
#include <QAction>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <limits>

namespace {
QImage card(int w = 64, int h = 48) {
    QImage im(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            QRgb c;
            if (y < h / 3) { int v = x * 255 / (w - 1); c = qRgb(v, v, v); }
            else if (y < 2 * h / 3) c = x < w / 3 ? qRgb(200, 40, 40) : x < 2 * w / 3 ? qRgb(40, 180, 60) : qRgb(30, 60, 210);
            else c = (x / 8 + y / 8) % 2 ? qRgb(230, 220, 120) : qRgb(20, 30, 50);
            im.setPixel(x, y, c);
        }
    return im;
}
QImage noisy(int w, int h, quint32 seed) {
    QImage im(w, h, QImage::Format_ARGB32);
    quint32 s = seed;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            s = s * 1664525u + 1013904223u;
            const int n = int((s >> 24) % 41) - 20;
            const int base = x < w / 2 ? 60 : 190;
            const int v = qBound(0, base + n, 255);
            im.setPixel(x, y, qRgb(v, v, v));
        }
    return im;
}
QImage argb(const QImage &i) { return i.convertToFormat(QImage::Format_ARGB32); }
bool same(const QImage &a, const QImage &b) { return argb(a) == argb(b); }
double variance(const QImage &im, QRect r) {
    const QImage a = argb(im);
    double s = 0, s2 = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y) for (int x = r.left(); x <= r.right(); ++x) { const double v = qGray(a.pixel(x, y)); s += v; s2 += v * v; ++n; }
    const double m = s / n;
    return s2 / n - m * m;
}
int countAlpha(const QImage &im) {
    const QImage a = argb(im);
    int n = 0;
    for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) n += qAlpha(a.pixel(x, y)) > 0;
    return n;
}
VObject bitmapObj(Document *d, const QImage &img, QPointF at = {}) {
    VObject o;
    o.id = d->newId();
    o.type = ObjType::Bitmap;
    o.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    o.rect = QRectF(at, QSizeF(img.size()));
    return o;
}
bool writePng(const QImage &img, const QString &path) { return img.save(path, "PNG"); }
}

class V030Test : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setProperty("lw_testing", true); }

    // ---------------------------------------------------------------- filters
    void gradientMapAndAutoContrast() {
        const QVector<GradStop> stops = {{0, QColor(255, 0, 0)}, {1, QColor(0, 0, 255)}};
        const QImage g = fx2::gradientMap(card(), stops, false, 100);
        QVERIFY(same(g, fx2::gradientMap(card(), stops, false, 100)));
        QCOMPARE(argb(g).pixel(0, 0), qRgb(255, 0, 0));          // black -> first stop
        QCOMPARE(argb(g).pixel(63, 0), qRgb(0, 0, 255));         // white -> last stop
        const QImage rev = fx2::gradientMap(card(), stops, true, 100);
        QCOMPARE(argb(rev).pixel(0, 0), qRgb(0, 0, 255));
        // low contrast 100..150 grey ramp -> stretched to ~0..255
        QImage low(64, 4, QImage::Format_ARGB32);
        for (int x = 0; x < 64; ++x) for (int y = 0; y < 4; ++y) { int v = 100 + x * 50 / 63; low.setPixel(x, y, qRgb(v, v, v)); }
        const QImage ac = argb(fx2::autoContrast(low));
        QVERIFY(qRed(ac.pixel(0, 0)) < 10);
        QVERIFY(qRed(ac.pixel(63, 0)) > 245);
        QVERIFY(same(ac, fx2::autoContrast(low)));
    }

    void ditherMethodsAndTargets() {
        QCOMPARE(fx2::ditherMethodNames().size(), int(fx2::kDitherMethods));
        QCOMPARE(fx2::paletteTargetNames().size(), int(fx2::kPaletteTargets));
        const QImage src = card();
        for (int m = fx2::FloydSteinberg; m < fx2::kDitherMethods; ++m) {
            const QImage a = fx2::dither(src, m, fx2::PalMono, 2, {});
            QVERIFY2(same(a, fx2::dither(src, m, fx2::PalMono, 2, {})), qPrintable(fx2::ditherMethodNames()[m]));
            const QVector<QRgb> u = fx2::uniqueColors(a);
            QVERIFY(u.size() <= 2);
            for (QRgb c : u) QVERIFY(c == qRgb(0, 0, 0) || c == qRgb(255, 255, 255));
            // the grey ramp must contain both black and white pixels (it is dithered, not thresholded to one)
            int white = 0;
            for (int x = 20; x < 44; ++x) white += argb(a).pixel(x, 2) == qRgb(255, 255, 255);
            QVERIFY(white > 0 && white < 24);
        }
        // web palette: every channel a multiple of 0x33
        for (QRgb c : fx2::uniqueColors(fx2::dither(src, fx2::FloydSteinberg, fx2::PalWeb, 216, {})))
            QVERIFY(qRed(c) % 0x33 == 0 && qGreen(c) % 0x33 == 0 && qBlue(c) % 0x33 == 0);
        // RGB332: at most 8/8/4 levels per channel
        QSet<int> rs, bs;
        for (QRgb c : fx2::uniqueColors(fx2::dither(src, fx2::Atkinson, fx2::PalRGB332, 256, {}))) { rs << qRed(c); bs << qBlue(c); }
        QVERIFY(rs.size() <= 8 && bs.size() <= 4);
        // RGB565: at most 32 red levels
        QSet<int> r5;
        for (QRgb c : fx2::uniqueColors(fx2::dither(noisy(64, 64, 3), fx2::Jarvis, fx2::PalRGB565, 0, {}))) r5 << qRed(c);
        QVERIFY(r5.size() <= 32);
        // N colours (adaptive) and custom
        QVERIFY(fx2::uniqueColors(fx2::dither(src, fx2::Stucki, fx2::PalAdaptive, 8, {})).size() <= 8);
        const QVector<QRgb> custom = {qRgb(255, 0, 0), qRgb(0, 0, 0), qRgb(255, 255, 0)};
        for (QRgb c : fx2::uniqueColors(fx2::dither(src, fx2::Bayer8, fx2::PalCustom, 3, custom))) QVERIFY(custom.contains(c));
        // transparent pixels are untouched
        QImage t(8, 8, QImage::Format_ARGB32);
        t.fill(Qt::transparent);
        t.setPixel(3, 3, qRgb(120, 120, 120));
        QCOMPARE(countAlpha(fx2::dither(t, fx2::FloydSteinberg, fx2::PalMono, 2, {})), 1);
    }

    void paletteSwapAndUniqueColors() {
        QImage im(4, 1, QImage::Format_ARGB32);
        im.setPixel(0, 0, qRgb(255, 0, 0)); im.setPixel(1, 0, qRgb(0, 255, 0)); im.setPixel(2, 0, qRgb(255, 0, 0)); im.setPixel(3, 0, qRgb(1, 2, 3));
        QCOMPARE(fx2::uniqueColors(im), QVector<QRgb>({qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(1, 2, 3)}));
        const QImage s = argb(fx2::paletteSwap(im, {qRgb(255, 0, 0), qRgb(0, 255, 0)}, {qRgb(0, 0, 255), qRgb(9, 9, 9)}, false));
        QCOMPARE(s.pixel(0, 0), qRgb(0, 0, 255));
        QCOMPARE(s.pixel(1, 0), qRgb(9, 9, 9));
        QCOMPARE(s.pixel(3, 0), qRgb(1, 2, 3));   // exact mode: unmatched colours stay
        const QImage n = argb(fx2::paletteSwap(im, {qRgb(255, 0, 0), qRgb(0, 255, 0)}, {qRgb(0, 0, 255), qRgb(9, 9, 9)}, true));
        QVERIFY(n.pixel(3, 0) == qRgb(0, 0, 255) || n.pixel(3, 0) == qRgb(9, 9, 9));   // nearest: everything maps
    }

    void outlineAndPixelPerfect() {
        QImage dot(7, 7, QImage::Format_ARGB32);
        dot.fill(Qt::transparent);
        dot.setPixel(3, 3, qRgb(255, 255, 255));
        const QImage o4 = fx2::outline(dot, false, false, 1, Qt::black, 128);
        const QImage o8 = fx2::outline(dot, false, true, 1, Qt::black, 128);
        QCOMPARE(countAlpha(o4), 5);
        QCOMPARE(countAlpha(o8), 9);
        QCOMPARE(argb(o4).pixel(2, 3), qRgb(0, 0, 0));
        QCOMPARE(qAlpha(argb(o4).pixel(2, 2)), 0);
        QCOMPARE(argb(o4).pixel(3, 3), qRgb(255, 255, 255));
        QCOMPARE(countAlpha(fx2::outline(dot, false, true, 2, Qt::red, 128)), 25);
        // inner outline of a 5x5 square: its border ring (16 px) changes colour, the inside (9) not
        QImage sq(7, 7, QImage::Format_ARGB32);
        sq.fill(Qt::transparent);
        for (int y = 1; y < 6; ++y) for (int x = 1; x < 6; ++x) sq.setPixel(x, y, qRgb(255, 255, 255));
        const QImage in = argb(fx2::outline(sq, true, false, 1, Qt::black, 128));
        QCOMPARE(countAlpha(in), 25);
        QCOMPARE(in.pixel(1, 1), qRgb(0, 0, 0));
        QCOMPARE(in.pixel(3, 3), qRgb(255, 255, 255));
        // pixel perfect removes the L-corner pixel
        QCOMPARE(fx2::pixelPerfect({{0, 0}, {1, 0}, {1, 1}, {2, 1}}), QVector<QPoint>({{0, 0}, {1, 1}, {2, 1}}));
        QCOMPARE(fx2::pixelPerfect({{0, 0}, {1, 0}, {2, 0}}).size(), 3);   // straight lines are kept
        QCOMPARE(fx2::linePixels({0, 0}, {4, 2}).size(), 5);
    }

    void normalMapAoSeamless() {
        QImage flat(16, 16, QImage::Format_ARGB32);
        flat.fill(qRgb(128, 128, 128));
        const QImage n = argb(fx2::normalMap(flat, 0, 2, false, true));
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
            const QRgb c = n.pixel(x, y);
            QVERIFY(qAbs(qRed(c) - 128) <= 1 && qAbs(qGreen(c) - 128) <= 1 && qBlue(c) >= 254);
        }
        // a left-to-right ramp tilts the normal: red channel deviates; Scharr / Sobel both deterministic
        const QImage ramp = card();
        const QImage sob = argb(fx2::normalMap(ramp, 0, 2, false, false)), sch = argb(fx2::normalMap(ramp, 1, 2, false, false));
        QVERIFY(qAbs(qRed(sob.pixel(30, 5)) - 128) > 3);
        QVERIFY(same(sob, fx2::normalMap(ramp, 0, 2, false, false)));
        QVERIFY(!same(sob, sch));
        // invert Y flips the green deviation
        QImage vr(32, 32, QImage::Format_ARGB32);
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) vr.setPixel(x, y, qRgb(y * 8, y * 8, y * 8));
        const int g1 = qGreen(argb(fx2::normalMap(vr, 0, 2, false, false)).pixel(16, 16)) - 128;
        const int g2 = qGreen(argb(fx2::normalMap(vr, 0, 2, true, false)).pixel(16, 16)) - 128;
        QVERIFY(g1 * g2 < 0);
        // AO of a flat image is uniform; a pit is darker than the plateau
        const QImage ao = argb(fx2::ambientOcclusion(flat, 4, 100, true));
        QCOMPARE(fx2::uniqueColors(ao).size(), 1);
        QImage pit(32, 32, QImage::Format_ARGB32);
        pit.fill(qRgb(255, 255, 255));
        for (int y = 12; y < 20; ++y) for (int x = 12; x < 20; ++x) pit.setPixel(x, y, qRgb(0, 0, 0));
        const QImage pao = argb(fx2::ambientOcclusion(pit, 6, 100, false));
        QVERIFY(qGray(pao.pixel(16, 16)) < qGray(pao.pixel(2, 2)));
        // seamless: the wrap-around seam gets smaller in both modes
        const QImage src = card();
        auto seam = [](const QImage &i) { const QImage a = argb(i); int d = 0; for (int y = 0; y < a.height(); ++y) d += qAbs(qGray(a.pixel(0, y)) - qGray(a.pixel(a.width() - 1, y))); return d; };
        for (int mode = 0; mode < 2; ++mode) {
            const QImage s = fx2::makeSeamless(src, mode, 25);
            QCOMPARE(s.size(), src.size());
            QVERIFY2(seam(s) < seam(src), qPrintable(QString::number(mode)));
            QVERIFY(same(s, fx2::makeSeamless(src, mode, 25)));
        }
    }

    void photoFilters() {
        const QImage src = card();
        double p0[8] = {0, 0, 0, 0, 50, 0, 0, 0};
        QVERIFY(same(fx2::lensCorrection(src, p0), src));
        double p1[8] = {30, 20, 0, -60, 50, 0, 0, 0};
        const QImage lc = fx2::lensCorrection(src, p1);
        QVERIFY(!same(lc, src));
        QVERIFY(same(lc, fx2::lensCorrection(src, p1)));
        QVERIFY(qGray(argb(lc).pixel(0, 47)) <= qGray(src.pixel(0, 47)) + 2);   // vignette darkens a corner
        // smart sharpen: more contrast across an edge
        const QImage soft = src.scaled(32, 24, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).scaled(64, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int k = 0; k < 3; ++k) {
            const QImage sh = fx2::smartSharpen(soft, 150, 2, k, 0, 0);
            QVERIFY(variance(sh, QRect(0, 16, 64, 16)) >= variance(soft, QRect(0, 16, 64, 16)));
            QVERIFY(same(sh, fx2::smartSharpen(soft, 150, 2, k, 0, 0)));
        }
        // surface blur / bilateral: smooth the noise inside each half, keep the edge between them
        const QImage nz = noisy(64, 32, 7);
        const QImage sb = argb(fx2::surfaceBlur(nz, 4, 40));
        const QImage bl = argb(fx2::bilateralDenoise(nz, 3, 40, 2));
        for (const QImage &r : {sb, bl}) {
            QVERIFY(variance(r, QRect(4, 4, 20, 24)) < variance(nz, QRect(4, 4, 20, 24)) * 0.6);
            QVERIFY(qGray(r.pixel(29, 16)) < 110 && qGray(r.pixel(34, 16)) > 140);   // edge kept
        }
        QVERIFY(same(sb, fx2::surfaceBlur(nz, 4, 40)));
        QVERIFY(same(bl, fx2::bilateralDenoise(nz, 3, 40, 2)));
    }

    void posterizeThresholdGradientMapAsLiveEffects() {
        // as live effects on a vector: the rendered rectangle is recoloured but stays a Rect
        for (EffectType t : {EffectType::GradientMap, EffectType::Posterize, EffectType::Threshold, EffectType::Dither,
                             EffectType::Outline, EffectType::NormalMap, EffectType::SurfaceBlur}) {
            QVERIFY(allEffectTypes().contains(t));
            VObject o;
            o.id = 1; o.type = ObjType::Rect; o.rect = QRectF(10, 10, 40, 30); o.fillOn = true; o.fill = QColor(60, 120, 200);
            DocState s; s.width = 60; s.height = 50; s.layers = {LayerInfo()};
            Frame f; f.layers = {{o}}; s.frames = {f};
            const QImage plain = render::renderFrame(s, 0, false);
            LiveEffect e = LiveEffect::make(t);
            if (t == EffectType::Threshold) e.threshold = 128;
            s.frames[0].layers[0][0].effects = {e};
            const QImage fxd = render::renderFrame(s, 0, false);
            QVERIFY2(!same(plain, fxd), qPrintable(effectName(t)));
            QVERIFY(same(fxd, render::renderFrame(s, 0, false)));
            QCOMPARE(s.frames[0].layers[0][0].type, ObjType::Rect);
            bool ok = false;
            const LiveEffect back = lwd::effectFromJson(lwd::effectToJson(e), &ok);
            QVERIFY(ok);
            QCOMPARE(lwd::effectToJson(back), lwd::effectToJson(e));
        }
    }

    void contentAwareFillIsDeterministic() {
        // a stripe pattern with a white blob over it: the blob gets refilled with the pattern
        QImage im(48, 48, QImage::Format_ARGB32);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 48; ++x) im.setPixel(x, y, (x / 4) % 2 ? qRgb(30, 90, 160) : qRgb(40, 110, 180));
        QImage mask(48, 48, QImage::Format_Alpha8);
        mask.fill(0);
        for (int y = 18; y < 30; ++y) for (int x = 18; x < 30; ++x) { im.setPixel(x, y, qRgb(255, 255, 255)); }
        // Alpha8 setPixel stores into alpha: write bytes directly to be sure
        for (int y = 18; y < 30; ++y) memset(mask.scanLine(y) + 18, 255, 12);
        const QImage a = argb(fx2::contentAwareFill(im, mask, 5));
        const QImage b = argb(fx2::contentAwareFill(im, mask, 5));
        QVERIFY(a == b);
        for (int y = 18; y < 30; ++y) for (int x = 18; x < 30; ++x) QVERIFY(qRed(a.pixel(x, y)) < 100);   // no white left
        QCOMPARE(a.pixel(2, 2), im.pixel(2, 2));   // outside the mask untouched
        // cancel returns the input
        const QImage c = fx2::contentAwareFill(im, mask, 5, [](int) { return false; });
        QVERIFY(same(c, im));
    }


    // 0.3.1: the Content-Aware Remove crash. Holes wider than 14 px build an image pyramid; 0.3.0
    // kept a reference to its first level across the appends (use-after-free, segfault in release).
    void contentAwareFillCrashCases() {
        auto stroke = [](QSize sz, QLineF l, int width) {
            QImage m(sz, QImage::Format_Alpha8);
            m.fill(0);
            QPainter p(&m);
            p.setPen(QPen(Qt::black, width, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(l);
            return m;
        };
        auto checkOutside = [](const QImage &in, const QImage &out, const QImage &mask) {
            const QImage a = argb(in), b = argb(out), m = mask.convertToFormat(QImage::Format_Alpha8);
            for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x)
                if (m.constScanLine(y)[x] <= 127 && a.pixel(x, y) != b.pixel(x, y)) return false;
            return true;
        };
        // the reported case: a plain stroke on an ordinary bitmap (multi-level pyramid)
        {
            const QImage im = noisy(200, 150, 3);
            const QImage m = stroke(im.size(), QLineF(30, 70, 170, 80), 20);
            const QImage out = fx2::contentAwareFill(im, m, 1);
            QCOMPARE(out.size(), im.size());
            QVERIFY(checkOutside(im, out, m));
            QVERIFY(argb(out) == argb(fx2::contentAwareFill(im, m, 1)));   // still deterministic
        }
        // sizes around the patch (7) and margin (48), edges and corners, hole = whole image
        for (QSize sz : {QSize(1, 1), QSize(3, 2), QSize(6, 7), QSize(7, 7), QSize(15, 9), QSize(40, 47), QSize(48, 48), QSize(97, 50)}) {
            const QImage im = noisy(sz.width(), sz.height(), 7);
            const QVector<QImage> masks = {
                stroke(sz, QLineF(0, 0, sz.width(), sz.height()), 5),
                stroke(sz, QLineF(0, 0, 0, 0), 3),
                stroke(sz, QLineF(sz.width() - 1, 0, sz.width() - 1, sz.height() - 1), 4),
                stroke(sz, QLineF(0, sz.height() / 2.0, sz.width(), sz.height() / 2.0), 2000),
            };
            for (const QImage &m : masks) {
                const QImage out = fx2::contentAwareFill(im, m, 2);
                QCOMPARE(out.size(), sz);
                QVERIFY(checkOutside(im, out, m));
            }
        }
        // other pixel / mask formats, a mask of another size, a transparent bitmap, null input
        for (QImage::Format f : {QImage::Format_Indexed8, QImage::Format_Grayscale8, QImage::Format_RGB888, QImage::Format_RGB32, QImage::Format_Mono}) {
            const QImage im = card(120, 90).convertToFormat(f);
            QCOMPARE(fx2::contentAwareFill(im, stroke(im.size(), QLineF(10, 10, 110, 80), 18), 1).size(), im.size());
        }
        QCOMPARE(fx2::contentAwareFill(card(120, 90), stroke(QSize(60, 45), QLineF(5, 5, 55, 40), 9), 1).size(), QSize(120, 90));
        QCOMPARE(fx2::contentAwareFill(card(120, 90), stroke(QSize(120, 90), QLineF(5, 5, 115, 85), 16).convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_ARGB32), 1).size(), QSize(120, 90));
        QImage clear(150, 100, QImage::Format_ARGB32_Premultiplied);
        clear.fill(Qt::transparent);
        QCOMPARE(fx2::contentAwareFill(clear, stroke(clear.size(), QLineF(10, 50, 140, 50), 30), 1).size(), clear.size());
        QVERIFY(fx2::contentAwareFill(QImage(), QImage(), 1).isNull());
        QCOMPARE(fx2::contentAwareFill(card(), QImage(), 1).size(), card().size());
        // a hole larger than the work limit is filled at a reduced scale (bounded time / memory)
        {
            QImage big(2400, 1800, QImage::Format_ARGB32_Premultiplied);
            big.fill(QColor(30, 90, 160));
            QImage m(big.size(), QImage::Format_Alpha8);
            m.fill(0);
            { QPainter p(&m); p.fillRect(QRect(600, 400, 1200, 1000), Qt::black); }
            const QImage out = argb(fx2::contentAwareFill(big, m, 1));
            QCOMPARE(out.size(), big.size());
            QCOMPARE(out.pixel(10, 10), argb(big).pixel(10, 10));
            QCOMPARE(qBlue(out.pixel(1200, 900)), 160);   // filled from the surrounding colour
        }
    }

    void liquifyEdgeCases() {
        // empty and one-pixel images, strokes off the image, non-finite and huge input, bad modes
        for (QSize sz : {QSize(0, 0), QSize(1, 1), QSize(2, 1), QSize(5, 40)}) {
            QImage im(sz, QImage::Format_ARGB32);
            if (!im.isNull()) im.fill(Qt::green);
            fx2::Liquify l(im);
            const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
            for (int m = -1; m <= fx2::Liquify::kModes; ++m) {
                l.stroke(fx2::Liquify::Mode(m), QPointF(-50, -50), QPointF(1e9, -1e9), 30, 1);
                l.stroke(fx2::Liquify::Mode(m), QPointF(0, 0), QPointF(sz.width(), sz.height()), 1e12, 0.7);
                l.stroke(fx2::Liquify::Mode(m), QPointF(nan, 0), QPointF(1, 1), 10, 0.5);
                l.stroke(fx2::Liquify::Mode(m), QPointF(0, 0), QPointF(1, 1), inf, 0.5);
                l.stroke(fx2::Liquify::Mode(m), QPointF(0, 0), QPointF(1, 1), 10, nan);
                l.stroke(fx2::Liquify::Mode(m), QPointF(0, 0), QPointF(inf, 1), 10, 0.5);
            }
            QCOMPARE(l.render().size(), im.size());
            QCOMPARE(l.displacement(-1, 0), QPointF());
            QCOMPARE(l.displacement(sz.width(), sz.height()), QPointF());
        }
        // the dialog with a 1x1 bitmap and strokes far outside it
        LiquifyDialog dlg(card(1, 1));
        for (int m = 0; m < fx2::Liquify::kModes; ++m) {
            dlg.setMode(m);
            dlg.strokeImage(QPointF(-100, -100), QPointF(100, 100));
        }
        QCOMPARE(dlg.liquified().size(), QSize(1, 1));
    }

    // Content-Aware Fill (Shift+Backspace) through MainWindow: selections at the edge, outside the bitmap,
    // over all of it, on a small bitmap, transparent bitmap and vector objects; undo afterwards
    void contentAwareFillCommand() {
        MainWindow w;
        DocumentView *v = w.addDocument(Document::createNew(300, 200, Qt::white));
        Document *d = v->doc();
        auto addBm = [d](const QImage &img, QPointF at) {
            VObject bm = bitmapObj(d, img, at);
            d->addObject(bm);
            d->selectOnly(bm.id);
            d->commit(QStringLiteral("add"));
            return bm.id;
        };
        auto sel = [d](QRectF r) { QPainterPath p; p.addRect(r); d->pixelSelection = p; };
        const quint64 id = addBm(noisy(200, 150, 9), QPointF(50, 25));
        const QImage orig = d->find(id)->image;
        sel(QRectF(40, 20, 60, 50));   // crosses the top-left corner of the bitmap
        QVERIFY(w.contentAwareFill(3));
        QCOMPARE(d->find(id)->image.size(), QSize(200, 150));
        d->undo();
        QVERIFY(d->find(id)->image == orig);
        sel(QRectF(0, 0, 300, 200));   // everything
        QVERIFY(w.contentAwareFill(3));
        d->undo();
        sel(QRectF(260, 180, 30, 15)); // outside the bitmap: nothing to do
        QVERIFY(!w.contentAwareFill(3));
        QVERIFY(d->find(id)->image == orig);
        const quint64 small = addBm(card(9, 6), QPointF(5, 5));
        sel(QRectF(4, 4, 6, 6));
        QVERIFY(w.contentAwareFill(3));
        QCOMPARE(d->find(small)->image.size(), QSize(9, 6));
        QImage clear(80, 60, QImage::Format_ARGB32_Premultiplied);
        clear.fill(Qt::transparent);
        addBm(clear, QPointF(150, 100));
        sel(QRectF(160, 110, 40, 30));
        w.contentAwareFill(3);   // no crash; may or may not change anything
        VObject r; r.id = d->newId(); r.type = ObjType::Rect; r.rect = QRectF(10, 10, 50, 40);
        d->addObject(r); d->selectOnly(r.id); d->commit(QStringLiteral("rect"));
        sel(QRectF(15, 15, 20, 20));
        QVERIFY(!w.contentAwareFill(3));   // a vector object is not a target
        d->pixelSelection = QPainterPath();
        QVERIFY(!w.contentAwareFill(3));   // no selection
        d->markClean();
    }

    void liquifyModes() {
        fx2::Liquify lq(card());
        QVERIFY(lq.isIdentity());
        QVERIFY(same(lq.render(), card()));
        QCOMPARE(fx2::Liquify::modeNames().size(), int(fx2::Liquify::kModes));
        for (int m = fx2::Liquify::ForwardWarp; m < fx2::Liquify::Reconstruct; ++m) {
            fx2::Liquify l(card());
            l.stroke(fx2::Liquify::Mode(m), QPointF(30, 20), QPointF(36, 22), 12, 0.8);
            QVERIFY2(!l.isIdentity(), qPrintable(fx2::Liquify::modeNames()[m]));
            QVERIFY(!same(l.render(), card()));
            fx2::Liquify l2(card());
            l2.stroke(fx2::Liquify::Mode(m), QPointF(30, 20), QPointF(36, 22), 12, 0.8);
            QVERIFY(same(l.render(), l2.render()));
            // reconstruct brings it back towards the original
            const double before = std::hypot(l.displacement(33, 21).x(), l.displacement(33, 21).y());
            for (int k = 0; k < 20; ++k) l.stroke(fx2::Liquify::Reconstruct, QPointF(33, 21), QPointF(33, 21), 30, 1);
            QVERIFY(std::hypot(l.displacement(33, 21).x(), l.displacement(33, 21).y()) < before * 0.5 + 1e-9);
        }
        LiquifyDialog dlg(card());
        dlg.setMode(fx2::Liquify::Bloat);
        dlg.strokeImage(QPointF(32, 24), QPointF(32, 24));
        QVERIFY(!dlg.liquify().isIdentity());
        QVERIFY(dlg.brushSize() > 0 && dlg.pressure() > 0);
    }

    // ---------------------------------------------------------------- C header dither
    void cHeaderDither() {
        Document *d = Document::createNew(64, 48, Qt::white);
        d->addObject(bitmapObj(d, card()));
        ExportOptions o = exportpresets::apply(exportpresets::indexOf(QStringLiteral("C Header RGB565 (embedded)")), d->exportOptions);
        QCOMPARE(o.format, Fmt::CHeader);
        QByteArray plain, dith;
        QString err;
        QVERIFY2(docio::exportDocument(*d, 0, o, plain, &err), qPrintable(err));
        o.chDither = fx2::FloydSteinberg;
        QVERIFY2(docio::exportDocument(*d, 0, o, dith, &err), qPrintable(err));
        QVERIFY(plain != dith);
        QByteArray again;
        QVERIFY(docio::exportDocument(*d, 0, o, again, &err));
        QCOMPARE(again, dith);
        // indexed and mono with dithering
        for (const char *p : {"C Header Indexed 8-bit", "C Header 1-bit mono (OLED)"}) {
            ExportOptions q = exportpresets::apply(exportpresets::indexOf(QString::fromLatin1(p)), d->exportOptions);
            QByteArray a, b;
            QVERIFY(docio::exportDocument(*d, 0, q, a, &err));
            q.chDither = fx2::Bayer4;
            QVERIFY(docio::exportDocument(*d, 0, q, b, &err));
            QVERIFY2(a != b, p);
        }
        // saved with the document
        QTemporaryDir tmp;
        d->exportOptions = o;
        const QString path = tmp.filePath(QStringLiteral("d.lwd"));
        QVERIFY(lwd::save(*d, path, &err));
        std::unique_ptr<Document> back(lwd::load(path, &err));
        QVERIFY(back);
        QCOMPARE(back->exportOptions.chDither, int(fx2::FloydSteinberg));
        delete d;
    }

    // ---------------------------------------------------------------- slices / image map
    void slicesAndImageMap() {
        Document *d = Document::createNew(100, 60, Qt::white);
        d->addObject(bitmapObj(d, card(100, 60)));
        const int web = d->ensureWebLayer();
        VObject s1; s1.id = d->newId(); s1.type = ObjType::Slice; s1.rect = QRectF(0, 0, 50, 30); s1.name = QStringLiteral("logo"); s1.link = QStringLiteral("https://example.com/"); s1.alt = QStringLiteral("Logo");
        VObject s2 = s1; s2.id = d->newId(); s2.rect = QRectF(40, 20, 40, 30); s2.name = QStringLiteral("button"); s2.link.clear(); // overlaps s1
        VObject h; h.id = d->newId(); h.type = ObjType::Hotspot; h.webShape = WebShape::Polygon;
        h.points = {QPointF(60, 5), QPointF(90, 5), QPointF(75, 25)};
        h.rect = QRectF(60, 5, 30, 20); h.link = QStringLiteral("page.html"); h.alt = QStringLiteral("Tri"); h.target = QStringLiteral("_blank");
        d->addObject(s1, web); d->addObject(s2, web); d->addObject(h, web);
        QTemporaryDir tmp;
        QString err;
        QStringList written;
        QVERIFY2(html::exportSlices(*d, tmp.path(), &err, &written), qPrintable(err));
        QCOMPARE(written.size(), 2);   // overlapping slices both exported
        for (const QString &f : written) QVERIFY(QFileInfo::exists(f));
        QVERIFY(written.join(QLatin1Char(' ')).contains(QLatin1String("logo")));
        written.clear();
        const QString page = tmp.filePath(QStringLiteral("map.html"));
        html::Options o;
        QVERIFY2(html::exportImageMap(*d, page, o, &err, &written), qPrintable(err));
        QFile f(page);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString htmlText = QString::fromUtf8(f.readAll());
        QVERIFY(htmlText.contains(QLatin1String("<map")));
        QVERIFY(htmlText.contains(QLatin1String("usemap")));
        QVERIFY(htmlText.contains(QLatin1String("shape=\"poly\"")));
        QVERIFY(htmlText.contains(QLatin1String("href=\"https://example.com/\"")));
        QVERIFY(htmlText.contains(QLatin1String("target=\"_blank\"")));
        QVERIFY(htmlText.contains(QLatin1String("alt=\"Tri\"")));
        QVERIFY(html::areaTag(h).contains(QLatin1String("coords=\"60,5,90,5,75,25\"")));
        delete d;
    }

    // ---------------------------------------------------------------- batch
    void batchScriptJson() {
        batch::Script s;
        s.input = batch::Script::Folder; s.folder = QStringLiteral("/tmp/in"); s.recursive = true;
        s.output = batch::Script::Backup; s.backupDir = QStringLiteral("orig");
        for (int t = 0; t < batch::kStepTypes; ++t) {
            batch::Step st; st.type = t;
            st.preset = QStringLiteral("PNG 32"); st.format = QStringLiteral("jpeg"); st.scaleMode = 2; st.width = 10; st.height = 20; st.onlyShrink = true;
            st.from = Qt::red; st.to = Qt::blue; st.tolerance = 12; st.find = QStringLiteral("a"); st.replace = QStringLiteral("b");
            st.prefix = QStringLiteral("p_"); st.numbering = true; st.digits = 4; st.command = QStringLiteral("flip-h");
            st.effects = {LiveEffect::make(EffectType::Invert), LiveEffect::make(EffectType::Dither)};
            s.steps << st;
            QCOMPARE(batch::stepTypeFromKey(batch::stepTypeKey(t)), t);
            QVERIFY(!st.summary().isEmpty());
        }
        s.steps[2].enabled = false;
        QString err;
        const batch::Script b = batch::Script::fromJson(s.toJson(), &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(QJsonDocument(b.toJson()).toJson(), QJsonDocument(s.toJson()).toJson());
        QVERIFY(!b.steps[2].enabled);
        QCOMPARE(b.steps[batch::Filters].effects.size(), 2);
        QTemporaryDir tmp;
        QVERIFY(s.save(tmp.filePath(QStringLiteral("s.json")), &err));
        bool ok = false;
        QCOMPARE(QJsonDocument(batch::Script::load(tmp.filePath(QStringLiteral("s.json")), &err, &ok).toJson()).toJson(), QJsonDocument(s.toJson()).toJson());
        QVERIFY(ok);
        QCOMPARE(batch::commandKeys().size(), batch::commandLabels().size());
    }

    void batchRunHeadless() {
        QTemporaryDir tmp;
        QDir in(tmp.filePath(QStringLiteral("in")));
        QVERIFY(QDir().mkpath(in.path()));
        QVERIFY(writePng(card(64, 48), in.filePath(QStringLiteral("a.png"))));
        QVERIFY(writePng(card(40, 40), in.filePath(QStringLiteral("b.png"))));
        QFile junk(in.filePath(QStringLiteral("notes.txt"))); QVERIFY(junk.open(QIODevice::WriteOnly)); junk.write("x"); junk.close();
        batch::Script s;
        s.input = batch::Script::Folder; s.folder = in.path(); s.patterns = QStringLiteral("*.png");
        s.output = batch::Script::ToFolder; s.outDir = tmp.filePath(QStringLiteral("out"));
        batch::Step sc; sc.type = batch::Scale; sc.scaleMode = 1; sc.percent = 50;
        batch::Step fl; fl.type = batch::Filters; fl.effects = {LiveEffect::make(EffectType::Invert)};
        batch::Step rn; rn.type = batch::Rename; rn.prefix = QStringLiteral("icon_"); rn.numbering = true; rn.start = 1; rn.digits = 2;
        batch::Step ex; ex.type = batch::Export; ex.preset = QStringLiteral("C Header RGB565 (embedded)");
        batch::Step ex2; ex2.type = batch::Export; ex2.preset = QStringLiteral("PNG 32");
        s.steps = {sc, fl, rn, ex, ex2};
        QCOMPARE(batch::inputFiles(s).size(), 2);
        int calls = 0;
        const batch::Result r = batch::run(s, {}, [&calls](int, int, const QString &) { ++calls; return true; });
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(r.processed, 2);
        QVERIFY(calls >= 2);
        const QStringList hs = QDir(s.outDir).entryList({QStringLiteral("*.h")}, QDir::Files);
        const QStringList ps = QDir(s.outDir).entryList({QStringLiteral("*.png")}, QDir::Files);
        QCOMPARE(hs.size(), 2);
        QCOMPARE(ps.size(), 2);
        for (const QString &f : hs + ps) QVERIFY2(f.startsWith(QLatin1String("icon_")), qPrintable(f));
        // the first file (a.png, 64x48) at 50 %, inverted
        const QStringList sorted = [&] { QStringList l = ps; l.sort(); return l; }();
        QImage outA(QDir(s.outDir).filePath(sorted.first()));
        QCOMPARE(outA.size(), QSize(32, 24));
        QVERIFY(qGray(outA.pixel(0, 1)) > 200);   // the black end of the ramp is now white
        QFile hf(QDir(s.outDir).filePath(hs.first()));
        QVERIFY(hf.open(QIODevice::ReadOnly));
        QVERIFY(hf.readAll().contains("32"));
        // cancel after the first file
        const batch::Result c = batch::run(s, {}, [](int i, int, const QString &) { return i < 1; });
        QVERIFY(c.cancelled);
        QVERIFY(c.processed <= 1);
    }

    void batchBackupAndOverwrite() {
        QTemporaryDir tmp;
        const QString a = tmp.filePath(QStringLiteral("a.png"));
        QVERIFY(writePng(card(20, 10), a));
        batch::Script s;
        s.input = batch::Script::Files; s.files = {a};
        s.output = batch::Script::Backup;
        batch::Step sc; sc.type = batch::Scale; sc.scaleMode = 2; sc.width = 10; sc.height = 10; sc.onlyShrink = true;
        batch::Step cmd; cmd.type = batch::Command; cmd.command = QStringLiteral("flip-h");
        s.steps = {sc, cmd};
        batch::Result r = batch::run(s);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(QImage(a).size(), QSize(10, 5));     // fit within 10x10 keeps the aspect
        QCOMPARE(QImage(tmp.filePath(QStringLiteral("Original Files/a.png"))).size(), QSize(20, 10));
        // again: the second backup gets a unique name, the first is kept
        r = batch::run(s);
        QVERIFY(r.errors.isEmpty());
        QCOMPARE(QDir(tmp.filePath(QStringLiteral("Original Files"))).entryList(QDir::Files).size(), 2);
        // overwrite + convert to JPEG: the PNG stays, a JPEG appears next to it
        s.output = batch::Script::Overwrite;
        batch::Step cv; cv.type = batch::Convert; cv.format = QStringLiteral("jpeg");
        s.steps = {cv};
        r = batch::run(s);
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QVERIFY(QFileInfo::exists(tmp.filePath(QStringLiteral("a.jpg"))) || QFileInfo::exists(tmp.filePath(QStringLiteral("a.jpeg"))));
        // ToFolder without a folder is an error, not a crash
        s.output = batch::Script::ToFolder; s.outDir.clear();
        r = batch::run(s);
        QVERIFY(!r.errors.isEmpty());
    }

    void batchReplaceColorAndText() {
        Document *d = Document::createNew(40, 40, Qt::white);
        VObject rect; rect.id = d->newId(); rect.type = ObjType::Rect; rect.rect = QRectF(0, 0, 20, 20); rect.fillOn = true; rect.fill = QColor(255, 0, 0);
        VObject txt; txt.id = d->newId(); txt.type = ObjType::Text; txt.rect = QRectF(0, 20, 40, 20); txt.text = QStringLiteral("Hello World");
        QImage bm(4, 4, QImage::Format_ARGB32); bm.fill(qRgb(250, 2, 1));
        d->addObject(rect); d->addObject(txt); d->addObject(bitmapObj(d, bm, QPointF(30, 30)));
        batch::Step rc; rc.type = batch::ReplaceColor; rc.from = QColor(255, 0, 0); rc.to = QColor(0, 0, 255); rc.tolerance = 6;
        QString err;
        QVERIFY(batch::applyStep(*d, rc, &err));
        const auto &objs = d->s.frames[0].layers[d->curLayer];
        QCOMPARE(objs[0].fill, QColor(0, 0, 255));
        QCOMPARE(argb(objs[2].image).pixel(1, 1), qRgb(0, 0, 255));
        batch::Step rt; rt.type = batch::ReplaceText; rt.find = QStringLiteral("world"); rt.replace = QStringLiteral("There"); rt.caseSensitive = false;
        QVERIFY(batch::applyStep(*d, rt, &err));
        QCOMPARE(objs[1].text, QStringLiteral("Hello There"));
        // filters on vectors stay live effects
        batch::Step fs; fs.type = batch::Filters; fs.effects = {LiveEffect::make(EffectType::GaussianBlur)};
        QVERIFY(batch::applyStep(*d, fs, &err));
        QCOMPARE(objs[0].type, ObjType::Rect);
        QVERIFY(!objs[0].effects.isEmpty());
        delete d;
    }

    void batchCli() {
        QTemporaryDir tmp;
        const QString a = tmp.filePath(QStringLiteral("a.png"));
        QVERIFY(writePng(card(30, 30), a));
        batch::Script s;
        batch::Step sc; sc.type = batch::Scale; sc.scaleMode = 0; sc.width = 15;
        batch::Step ex; ex.type = batch::Export; ex.preset = QStringLiteral("C Header 1-bit mono (OLED)");
        s.steps = {sc, ex};
        s.output = batch::Script::ToFolder; s.outDir = QStringLiteral("out");   // relative to the script
        const QString script = tmp.filePath(QStringLiteral("job.json"));
        QVERIFY(s.save(script));
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.start(QStringLiteral(LW_EXE), {QStringLiteral("--batch"), script, a});
        QVERIFY(p.waitForFinished(60000));
        QVERIFY2(p.exitCode() == 0, p.readAllStandardError().constData());
        QVERIFY(QFileInfo::exists(tmp.filePath(QStringLiteral("out/a.h"))));
        // a broken script fails with exit code 2
        QFile bad(tmp.filePath(QStringLiteral("bad.json"))); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("{nope"); bad.close();
        p.start(QStringLiteral(LW_EXE), {QStringLiteral("--batch"), bad.fileName(), a});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 2);
    }

    // ---------------------------------------------------------------- auto shapes
    void autoShapesAll() {
        const QStringList kinds = autoshape::kinds();
        QVERIFY(kinds.size() >= 16);
        for (const char *k : {"roundrect", "chamfer", "bevel", "polygon", "star", "arc", "spiral", "arrow", "arrowline",
                              "connector", "smartpolygon", "gear", "frame", "doughnut", "lshape", "speech"})
            QVERIFY2(autoshape::isKind(QString::fromLatin1(k)) || kinds.join(',').contains(QString::fromLatin1(k), Qt::CaseInsensitive), k);
        QTemporaryDir tmp;
        for (const QString &k : kinds) {
            QVERIFY(!autoshape::label(k).isEmpty());
            const QVector<autoshape::Param> ps = autoshape::params(k);
            QVERIFY2(!ps.isEmpty(), qPrintable(k));
            const QMap<QString, double> defs = autoshape::defaults(k);
            for (const auto &p : ps) {
                QVERIFY(defs.contains(p.key));
                QVERIFY2(p.def >= p.min && p.def <= p.max, qPrintable(k + QLatin1Char('.') + p.key));
            }
            Document *d = Document::createNew(200, 200, Qt::white);
            VObject o;
            o.id = d->newId(); o.type = ObjType::AutoShape; o.rect = QRectF(20, 30, 120, 90);
            o.fillOn = true; o.fill = QColor(10, 120, 200); o.strokeOn = true; o.stroke = QColor(0, 0, 0);
            autoshape::init(o, k);
            if (autoshape::isLineKind(k)) autoshape::setEndpoints(o, QPointF(20, 30), QPointF(140, 120));
            QCOMPARE(o.shapeKind, k);
            QVERIFY2(!autoshape::outline(o).isEmpty(), qPrintable(k));
            // setParam clamps to the range
            autoshape::setParam(o, ps[0].key, ps[0].max + 1e6);
            QCOMPARE(autoshape::param(o, ps[0].key), ps[0].max);
            autoshape::setParam(o, ps[0].key, ps[0].def);
            // diamond handles: dragging one changes the shape
            const QVector<autoshape::Handle> hs = autoshape::handles(o);
            QVERIFY2(!hs.isEmpty(), qPrintable(k));
            const QPainterPath before = autoshape::outline(o);
            const QMap<QString, double> pb = o.shapeParams;
            bool changedAny = false;
            for (const auto &h : hs) {
                VObject t = o;
                for (QPointF delta : {QPointF(9, 7), QPointF(-9, -7), QPointF(15, 0), QPointF(0, 15)}) {
                    VObject u = o;
                    autoshape::dragHandle(u, h.id, h.pos + delta);
                    if (u.shapeParams != pb || autoshape::outline(u) != before) { changedAny = true; break; }
                }
                Q_UNUSED(t);
                if (changedAny) break;
            }
            QVERIFY2(changedAny, qPrintable(k));
            // non-default parameters survive .lwd save / load
            for (const auto &p : ps) if (p.choices.isEmpty() && !QStringList{QStringLiteral("x1"), QStringLiteral("y1"), QStringLiteral("x2"), QStringLiteral("y2")}.contains(p.key)) autoshape::setParam(o, p.key, p.min + (p.max - p.min) * 0.37);
            d->addObject(o);
            QString err;
            const QString path = tmp.filePath(k + QStringLiteral(".lwd"));
            QVERIFY2(lwd::save(*d, path, &err), qPrintable(err));
            std::unique_ptr<Document> back(lwd::load(path, &err));
            QVERIFY2(back, qPrintable(err));
            const VObject &b = back->s.frames[0].layers[back->curLayer].last();
            QCOMPARE(b.type, ObjType::AutoShape);
            QCOMPARE(b.shapeKind, k);
            for (auto it = o.shapeParams.begin(); it != o.shapeParams.end(); ++it)
                QVERIFY2(qAbs(b.shapeParams.value(it.key()) - it.value()) < 1e-6, qPrintable(k + QLatin1Char('.') + it.key()));
            QCOMPARE(render::renderFrame(back->s, 0, true), render::renderFrame(d->s, 0, true));
            // SVG: vector paths, no embedded raster
            bool pure = false;
            const QByteArray svg = docio::stateToSvg(d->s, 0, &pure);
            QVERIFY2(pure, qPrintable(k));
            QVERIFY2(svg.contains("<path"), qPrintable(k + QLatin1Char(' ') + QString::fromUtf8(svg.right(600))));
            QVERIFY(!svg.contains("data:image/png"));
            // PDF
            QByteArray pdf;
            QVERIFY(docio::writePdf(d->s, pdf, &err));
            QVERIFY(pdf.startsWith("%PDF"));
            // Convert to Paths
            QVERIFY(paths::canConvert(o));
            d->selectOnly(o.id);
            QVERIFY(paths::convertToPaths(*d) >= 1);
            bool anyPath = false;
            for (const VObject &x : d->s.frames[0].layers[d->curLayer]) anyPath |= x.type == ObjType::Path;
            QVERIFY2(anyPath, qPrintable(k));
            delete d;
        }
    }

    // ---------------------------------------------------------------- UI wiring
    void menusAndCommands() {
        MainWindow w;
        for (const char *id : {"file.exportslices", "file.exportimagemap", "file.batch", "modify.toindexed", "modify.editpalette",
                               "filter.autocontrast", "filter.gradientmap", "filter.surfaceblur", "filter.denoise", "filter.smartsharpen",
                               "filter.lens", "filter.liquify", "filter.contentaware", "filter.dither", "filter.outline", "filter.paletteswap",
                               "filter.normalmap", "filter.ao", "filter.makeseamless"})
            QVERIFY2(w.findChild<QAction *>(QString::fromLatin1(id)), id);
        DocumentView *v = w.addDocument(Document::createNew(64, 48, Qt::white));
        Document *d = v->doc();
        VObject bm = bitmapObj(d, card());
        const quint64 id = bm.id;
        d->addObject(bm);
        d->selectOnly(id);
        d->commit(QStringLiteral("add"));
        // Liquify (scripted)
        QVERIFY(w.liquifyFilter([](LiquifyDialog &dlg) { dlg.setMode(fx2::Liquify::ForwardWarp); dlg.strokeImage({16, 24}, {40, 24}); dlg.strokeImage({40, 10}, {20, 40}); }));
        QCOMPARE(d->history().at(d->historyIndex()).label, QStringLiteral("Liquify"));
        QVERIFY(!same(d->find(id)->image, card()));
        // Edit Palette: replace every colour by black -> single-colour bitmap
        d->find(id)->image = card().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QVector<QRgb> pal = fx2::uniqueColors(card(), 257);
        QVERIFY(pal.size() <= 256);
        QVector<QRgb> blk(pal.size(), qRgb(0, 0, 0));
        QVERIFY(w.editPalette(&blk));
        QCOMPARE(fx2::uniqueColors(argb(d->find(id)->image)), QVector<QRgb>({qRgb(0, 0, 0)}));
        // Content-aware fill needs a selection; then fills it
        d->find(id)->image = card().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QVERIFY(!w.contentAwareFill());
        QPainterPath sel; sel.addRect(QRectF(40, 36, 8, 8));
        d->combinePixelSelection(sel, Qt::NoModifier);
        const QImage beforeFill = argb(d->find(id)->image);
        QVERIFY(w.contentAwareFill(3));
        const QImage afterFill = argb(d->find(id)->image);
        QVERIFY(beforeFill != afterFill);
        QCOMPARE(afterFill.pixel(2, 2), beforeFill.pixel(2, 2));
        d->clearPixelSelection();
        // export slices / image map with explicit paths
        QTemporaryDir tmp;
        VObject s; s.id = d->newId(); s.type = ObjType::Slice; s.rect = QRectF(0, 0, 32, 24); s.name = QStringLiteral("part");
        d->addObject(s, d->ensureWebLayer());
        QVERIFY(w.exportSlices(tmp.path()));
        QVERIFY(QDir(tmp.path()).entryList(QDir::Files).join(' ').contains(QLatin1String("part")));
        QVERIFY(w.exportImageMap(tmp.filePath(QStringLiteral("m.html"))));
        QVERIFY(QFileInfo::exists(tmp.filePath(QStringLiteral("m.html"))));
        // Batch dialog over the open documents (copies; the open document stays untouched)
        const QImage keep = argb(d->find(id)->image);
        BatchDialog bd({qMakePair(QStringLiteral("Untitled"), d)}, tmp.path());
        batch::Script sc;
        sc.input = batch::Script::OpenDocuments;
        sc.output = batch::Script::ToFolder; sc.outDir = tmp.filePath(QStringLiteral("batch"));
        batch::Step st; st.type = batch::Scale; st.scaleMode = 1; st.percent = 25;
        batch::Step ex; ex.type = batch::Convert; ex.format = QStringLiteral("png");
        sc.steps = {st, ex};
        bd.setScript(sc);
        QCOMPARE(bd.script().steps.size(), 2);
        QCOMPARE(bd.script().input, int(batch::Script::OpenDocuments));
        const batch::Result r = bd.runBatch();
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join(QLatin1Char('\n'))));
        QCOMPARE(r.processed, 1);
        const QStringList outs = QDir(sc.outDir).entryList({QStringLiteral("*.png")}, QDir::Files);
        QCOMPARE(outs.size(), 1);
        QCOMPARE(QImage(QDir(sc.outDir).filePath(outs.first())).size(), QSize(16, 12));
        QCOMPARE(argb(d->find(id)->image), keep);
        // script save / load through the dialog
        QVERIFY(bd.saveScript(tmp.filePath(QStringLiteral("x.json"))));
        BatchDialog bd2({}, tmp.path());
        QVERIFY(bd2.loadScript(tmp.filePath(QStringLiteral("x.json"))));
        QCOMPARE(QJsonDocument(bd2.script().toJson()).toJson(), QJsonDocument(bd.script().toJson()).toJson());
    }
};

QTEST_MAIN(V030Test)
#include "v030_test.moc"
