// Filters / Live Effects / retouch tool tests: every filter is applied to a known image and must be
// deterministic and change the pixels in the expected way.
#include "core/Effects.h"
#include "core/Retouch.h"
#include "core/BrushEngine.h"
#include "core/Render.h"
#include "core/Document.h"
#include "io/LwdFormat.h"
#include "io/ImageIO.h"
#include <QRandomGenerator>
#include <QtTest>
#include <cmath>

namespace {
// 64x48 test card: horizontal grey ramp on top, colour patches below, all opaque.
QImage card() {
    QImage im(64, 48, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) {
            QRgb c;
            if (y < 16) { int v = x * 255 / 63; c = qRgb(v, v, v); }
            else if (y < 32) c = x < 21 ? qRgb(200, 40, 40) : x < 42 ? qRgb(40, 180, 60) : qRgb(30, 60, 210);
            else c = (x / 8 + y / 8) % 2 ? qRgb(230, 220, 120) : qRgb(20, 30, 50);
            im.setPixel(x, y, c);
        }
    return im;
}
QRgb px(const QImage &im, int x, int y) { return im.convertToFormat(QImage::Format_ARGB32).pixel(x, y); }
int changed(const QImage &a, const QImage &b) {
    QImage A = a.convertToFormat(QImage::Format_ARGB32), B = b.convertToFormat(QImage::Format_ARGB32);
    int n = 0;
    for (int y = 0; y < A.height(); ++y) for (int x = 0; x < A.width(); ++x) n += A.pixel(x, y) != B.pixel(x, y);
    return n;
}
QImage run(const LiveEffect &e, const QImage &src = card()) { QImage im = src; fx::adjust(im, e); return im; }
// opaque rectangle object for live effect tests
VObject rectObj() {
    VObject o;
    o.id = 1;
    o.type = ObjType::Rect;
    o.rect = QRectF(20, 20, 60, 40);
    o.fillOn = true;
    o.fill = QColor(60, 120, 200);
    return o;
}
QImage renderWith(const QVector<LiveEffect> &fx, QRect *area = nullptr) {
    Q_UNUSED(area);
    VObject o = rectObj();
    o.effects = fx;
    return render::renderFrame([&] { DocState s; s.width = 100; s.height = 80; Frame f; f.layers = {{o}}; s.frames = {f}; s.layers = {LayerInfo()}; return s; }(), 0, false);
}
}

class EffectsTest : public QObject {
    Q_OBJECT
private slots:
    void everyTypeHasKeyAndName() {
        QSet<QString> keys;
        for (EffectType t : allEffectTypes()) {
            QVERIFY(!effectName(t).isEmpty());
            QVERIFY(!effectCategory(t).isEmpty());
            bool ok = false;
            QCOMPARE(effectFromKey(effectKey(t), &ok), t);
            QVERIFY(ok);
            keys.insert(effectKey(t));
        }
        QCOMPARE(keys.size(), allEffectTypes().size());
        QCOMPARE(allEffectTypes().size(), int(EffectType::ReplaceColor) + 1); // every enum value is listed (ReplaceColor is last, 0.3.1)
    }

    // every adjustment: deterministic and changes the test card
    void adjustmentsChangeAndAreDeterministic_data() {
        QTest::addColumn<int>("type");
        for (EffectType t : allEffectTypes())
            if (effectIsAdjustment(t)) QTest::newRow(qPrintable(effectKey(t))) << int(t);
    }
    void adjustmentsChangeAndAreDeterministic() {
        QFETCH(int, type);
        LiveEffect e = LiveEffect::make(EffectType(type));
        // give "neutral by default" adjustments a non-trivial setting
        switch (e.type) {
        case EffectType::HueSaturation: e.hue = 90; break;
        case EffectType::Levels: e.levels[ChRGB].inBlack = 40; break;
        case EffectType::Curves: e.curves[ChRGB] = QPolygonF({QPointF(0, 0), QPointF(128, 190), QPointF(255, 255)}); break;
        case EffectType::BrightnessContrast: e.brightness = 30; break;
        case EffectType::ColorBalance: e.balance[1][0] = 60; break;
        case EffectType::Offset: e.offsetX = 7; e.offsetY = -3; break;
        case EffectType::LensCorrection: e.p[0] = 30; break;               // barrel
        case EffectType::PaletteSwap: e.palette = {qRgb(0, 0, 0)}; e.palette2 = {qRgb(255, 0, 0)}; break;
        default: break;
        }
        QImage src = card();
        if (e.type == EffectType::Outline) { // needs transparency to outline
            for (int y = 10; y < 20; ++y) for (int x = 10; x < 30; ++x) src.setPixel(x, y, 0);
        }
        if (e.type == EffectType::AutoLevels || e.type == EffectType::AutoContrast) { // the card already spans 0..255: use a low-contrast copy
            LiveEffect lv = LiveEffect::make(EffectType::Levels);
            lv.levels[ChRGB].outBlack = 60; lv.levels[ChRGB].outWhite = 190;
            fx::adjust(src, lv);
        }
        QImage a = run(e, src), b = run(e, src);
        QCOMPARE(a.size(), src.size());
        QCOMPARE(changed(a, b), 0);
        QVERIFY2(changed(a, src) > 0, "filter must change the test card");
        
    }

    void invertDesaturateThresholdPosterize() {
        QImage inv = run(LiveEffect::make(EffectType::Invert));
        QCOMPARE(px(inv, 0, 20), qRgb(55, 215, 215));
        QImage ds = run(LiveEffect::make(EffectType::Desaturate));
        for (int x = 0; x < 64; x += 7) { QRgb p = px(ds, x, 24); QCOMPARE(qRed(p), qGreen(p)); QCOMPARE(qGreen(p), qBlue(p)); }
        QImage th = run(LiveEffect::make(EffectType::Threshold));
        for (int x = 0; x < 64; ++x) { int v = qRed(px(th, x, 4)); QVERIFY(v == 0 || v == 255); QCOMPARE(v, x * 255 / 63 >= 128 ? 255 : 0); }
        LiveEffect pe = LiveEffect::make(EffectType::Posterize);
        pe.posterize = 3;
        QImage po = run(pe);
        QSet<int> vals;
        for (int x = 0; x < 64; ++x) vals.insert(qRed(px(po, x, 4)));
        QCOMPARE(vals, (QSet<int>{0, 128, 255}));
    }

    void levelsPerChannel() {
        LiveEffect e = LiveEffect::make(EffectType::Levels);
        QCOMPARE(changed(run(e), card()), 0); // identity by default
        e.levels[ChRed].inBlack = 100;
        QImage out = run(e);
        QRgb p = px(out, 0, 20); // (200,40,40)
        QCOMPARE(qRed(p), qRound((200 - 100) / 155.0 * 255));
        QCOMPARE(qGreen(p), 40);
        QCOMPARE(qBlue(p), 40);
        e = LiveEffect::make(EffectType::Levels);
        e.levels[ChRGB].outWhite = 128;
        QCOMPARE(qRed(px(run(e), 63, 4)), 128);
        e = LiveEffect::make(EffectType::Levels);
        e.levels[ChRGB].gamma = 2.0;
        QVERIFY(qRed(px(run(e), 32, 4)) > qRed(px(card(), 32, 4)) + 20); // gamma > 1 brightens midtones
    }

    void curvesPerChannel() {
        LiveEffect e = LiveEffect::make(EffectType::Curves);
        QCOMPARE(changed(run(e), card()), 0);
        e.curves[ChGreen] = QPolygonF({QPointF(0, 255), QPointF(255, 0)}); // invert green only
        QRgb p = px(run(e), 0, 20);
        QCOMPARE(qRed(p), 200);
        QCOMPARE(qGreen(p), 215);
        QCOMPARE(qBlue(p), 40);
        // monotone through its points
        QVector<uchar> lut = fx::curveLut(QPolygonF({QPointF(0, 0), QPointF(64, 128), QPointF(255, 255)}));
        QCOMPARE(int(lut[64]), 128);
        for (int i = 1; i < 256; ++i) QVERIFY(lut[i] >= lut[i - 1]);
    }

    void autoLevelsStretches() {
        QImage flat(32, 8, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < 32; ++x) for (int y = 0; y < 8; ++y) flat.setPixel(x, y, qRgb(80 + x * 3, 80 + x * 3, 80 + x * 3));
        QImage out = run(LiveEffect::make(EffectType::AutoLevels), flat);
        QCOMPARE(qRed(px(out, 0, 0)), 0);
        QCOMPARE(qRed(px(out, 31, 0)), 255);
        QVector<int> h = fx::histogram(flat, ChRGB);
        QCOMPARE(h[80], 8);
    }

    void hueSaturationColorize() {
        LiveEffect e = LiveEffect::make(EffectType::HueSaturation);
        e.saturation = -100;
        QRgb p = px(run(e), 0, 20);
        QCOMPARE(qRed(p), qGreen(p));
        e = LiveEffect::make(EffectType::HueSaturation);
        e.colorize = true;
        e.hue = 240; // blue
        e.saturation = 100;
        QColor c = QColor(px(run(e), 32, 4));
        QVERIFY(qAbs(c.hslHue() - 240) <= 2);
        QVERIFY(c.blue() > c.red());
    }

    void colorBalanceAndPhotoFilter() {
        LiveEffect e = LiveEffect::make(EffectType::ColorBalance);
        e.preserveLuminosity = false;
        e.balance[0][0] = 100; // shadows towards red
        QImage out = run(e);
        QVERIFY(qRed(px(out, 2, 4)) > qRed(px(card(), 2, 4)) + 50);  // dark: strongly affected
        QVERIFY(qRed(px(out, 63, 4)) - 255 == 0);                      // white stays
        LiveEffect pf = LiveEffect::make(EffectType::PhotoFilter);     // warming
        pf.preserveLuminosity = false;
        pf.amount = 100;
        QRgb w = px(run(pf), 63, 4);
        QVERIFY(qBlue(w) < 30);
        QVERIFY(qRed(w) > 200);
        pf.preserveLuminosity = true;
        QRgb l = px(run(pf), 40, 4), o = px(card(), 40, 4);
        QVERIFY(qAbs(qGray(l) - qGray(o)) <= 6);
    }

    void blurs() {
        QImage line(41, 41, QImage::Format_ARGB32_Premultiplied);
        line.fill(qRgb(0, 0, 0));
        for (int y = 0; y < 41; ++y) line.setPixel(20, y, qRgb(255, 255, 255));
        LiveEffect m = LiveEffect::make(EffectType::MotionBlur); // horizontal
        m.distance = 10;
        QImage mo = run(m, line);
        QVERIFY(qRed(px(mo, 24, 20)) > 0);   // spread sideways
        QVERIFY(qRed(px(mo, 20, 20)) < 255);
        LiveEffect mv = m;
        mv.angle = 90;                        // vertical: a vertical line stays put
        QImage mvo = run(mv, line);
        QCOMPARE(qRed(px(mvo, 24, 20)), 0);
        QCOMPARE(qRed(px(mvo, 20, 20)), 255);
        // radial (spin) blur keeps the centre, smears the rim
        LiveEffect r = LiveEffect::make(EffectType::RadialBlur);
        r.amount = 40;
        QImage ro = run(r, line);
        QCOMPARE(qRed(px(ro, 20, 20)), 255);
        QVERIFY(qRed(px(ro, 20, 1)) < 255);
        LiveEffect z = LiveEffect::make(EffectType::ZoomBlur); // zoom: radial streaks keep the line (it passes the centre)
        QImage zo = run(z, line);
        QCOMPARE(changed(zo, line), 0); // a line through the centre is invariant under zoom
        QImage zc = run(z, card());
        QVERIFY(changed(zc, card()) > 0);
        QCOMPARE(px(zc, 32, 24), px(card(), 32, 24)); // centre stays sharp
        // gaussian blur radius matters
        LiveEffect g1 = LiveEffect::make(EffectType::GaussianBlur), g2 = g1;
        g1.softness = 1; g2.softness = 6;
        QVERIFY(qRed(px(run(g2, line), 26, 20)) > qRed(px(run(g1, line), 26, 20)));
    }

    void sharpenAndNoise() {
        QImage soft = card();
        LiveEffect u = LiveEffect::make(EffectType::UnsharpMask);
        u.amount = 200;
        QImage us = run(u, soft);
        // edge contrast increases at the red/green boundary
        QVERIFY(qRed(px(us, 20, 24)) >= qRed(px(soft, 20, 24)));
        u.threshold = 255; // nothing exceeds the threshold
        QCOMPARE(changed(run(u, soft), soft), 0);
        LiveEffect n = LiveEffect::make(EffectType::Noise);
        QImage n1 = run(n), n2 = run(n);
        QCOMPARE(changed(n1, n2), 0);
        n.colorNoise = true;
        QRgb c = px(run(n), 5, 40);
        Q_UNUSED(c);
        QVERIFY(changed(run(n), n1) > 0);
    }

    void otherFilters() {
        QImage edges = run(LiveEffect::make(EffectType::FindEdges));
        QCOMPARE(px(edges, 10, 24), qRgb(255, 255, 255)); // flat patch -> white
        QVERIFY(qGray(px(edges, 21, 24)) < 200);          // patch boundary -> edge
        QImage a = run(LiveEffect::make(EffectType::ConvertToAlpha));
        QCOMPARE(qAlpha(px(a, 0, 4)), 255);   // black -> opaque
        QCOMPARE(qAlpha(px(a, 63, 4)), 0);    // white -> transparent
        QCOMPARE(qRed(px(a, 0, 4)), 0);
        LiveEffect f = LiveEffect::make(EffectType::ColorFill);
        f.opacity = 100;
        f.color = QColor(10, 20, 30);
        QCOMPARE(px(run(f), 33, 33), qRgb(10, 20, 30));
        // transparent pixels stay transparent
        QImage t(8, 8, QImage::Format_ARGB32_Premultiplied);
        t.fill(0);
        QCOMPARE(qAlpha(px(run(f, t), 3, 3)), 0);
    }

    // shadow / glow / bevel / emboss Live Effects
    void liveEffectsRender_data() {
        QTest::addColumn<int>("type");
        for (EffectType t : allEffectTypes())
            if (!effectIsAdjustment(t)) QTest::newRow(qPrintable(effectKey(t))) << int(t);
    }
    void liveEffectsRender() {
        QFETCH(int, type);
        const QImage plain = renderWith({});
        QImage a = renderWith({LiveEffect::make(EffectType(type))}), b = renderWith({LiveEffect::make(EffectType(type))});
        QCOMPARE(changed(a, b), 0);
        QVERIFY(changed(a, plain) > 0);
        LiveEffect off = LiveEffect::make(EffectType(type));
        off.enabled = false;
        QCOMPARE(changed(renderWith({off}), plain), 0); // disabled effects do nothing
    }

    void effectPlacement() {
        const QImage plain = renderWith({});
        // inner glow / inner shadow stay inside, outer bevel / glow go outside
        QImage ig = renderWith({LiveEffect::make(EffectType::InnerGlow)});
        QCOMPARE(qAlpha(px(ig, 15, 40)), 0);
        QVERIFY(px(ig, 21, 40) != px(plain, 21, 40));
        QImage ob = renderWith({LiveEffect::make(EffectType::OuterBevel)});
        QVERIFY(qAlpha(px(ob, 17, 40)) > 0);
        QCOMPARE(px(ob, 50, 40), px(plain, 50, 40)); // centre untouched
        // bevel lighting follows the angle: light from the top-left lights the top edge
        LiveEffect tl = LiveEffect::make(EffectType::InnerBevel);
        LiveEffect br = tl;
        br.angle = 315;
        QImage a = renderWith({tl}), b = renderWith({br});
        QVERIFY(qGray(px(a, 50, 21)) > qGray(px(plain, 50, 21)));
        QVERIFY(qGray(px(b, 50, 21)) < qGray(px(plain, 50, 21)));
        // inset flips the lighting
        LiveEffect in = tl;
        in.preset = BevelPreset::Inset;
        QVERIFY(qGray(px(renderWith({in}), 50, 21)) < qGray(px(plain, 50, 21)));
        // emboss without "show object" hides the fill
        LiveEffect em = LiveEffect::make(EffectType::RaisedEmboss);
        em.showObject = false;
        QCOMPARE(qAlpha(px(renderWith({em}), 50, 40)), 0);
    }

    void effectOrderMatters() {
        LiveEffect inv = LiveEffect::make(EffectType::Invert), th = LiveEffect::make(EffectType::Threshold);
        th.threshold = 100;
        // fill (60,120,200) grey ~ 112: threshold then invert = black; invert then threshold (grey ~143) = white
        QCOMPARE(qRed(px(renderWith({th, inv}), 50, 40)), 0);
        QCOMPARE(qRed(px(renderWith({inv, th}), 50, 40)), 255);
    }

    // Live Effects are a pipeline: each effect sees everything above it, glows and shadows included
    void hueAfterGlowTintsTheGlow() {
        LiveEffect glow = LiveEffect::make(EffectType::OuterGlow);
        glow.color = QColor(255, 0, 0); glow.width = 8; glow.softness = 0; glow.opacity = 100;
        LiveEffect hue = LiveEffect::make(EffectType::HueSaturation);
        hue.hue = 120;
        // (50,15) is 5 px above the rectangle: inside the glow
        const QRgb glowThenHue = px(renderWith({glow, hue}), 50, 15);
        const QRgb hueThenGlow = px(renderWith({hue, glow}), 50, 15);
        QVERIFY2(qGreen(glowThenHue) > qRed(glowThenHue) + 60 && qGreen(glowThenHue) > qBlue(glowThenHue) + 60,
                 qPrintable(QColor(glowThenHue).name()));
        QVERIFY2(qRed(hueThenGlow) > 200 && qGreen(hueThenGlow) < 60 && qBlue(hueThenGlow) < 60, qPrintable(QColor(hueThenGlow).name()));
        // the object itself is hue-shifted either way
        QCOMPARE(px(renderWith({glow, hue}), 50, 40), px(renderWith({hue, glow}), 50, 40));
        QVERIFY(px(renderWith({hue}), 50, 40) != px(renderWith({}), 50, 40));
    }
    void blurAfterGlowSpreadsTheGlow() {
        LiveEffect glow = LiveEffect::make(EffectType::OuterGlow);
        glow.color = QColor(255, 0, 0); glow.width = 8; glow.softness = 0;
        LiveEffect blur = LiveEffect::make(EffectType::GaussianBlur);
        blur.softness = 4;
        // 11 px above the rect: outside the hard glow, reached only when the blur acts on the glow
        const QRgb hard = px(renderWith({glow}), 50, 9), spread = px(renderWith({glow, blur}), 50, 9), under = px(renderWith({blur, glow}), 50, 9);
        // (renderWith has no canvas: compare coverage)
        QVERIFY2(qAlpha(hard) < 20, qPrintable(QString::number(qAlpha(hard))));
        QVERIFY2(qAlpha(spread) > qAlpha(hard) + 15, qPrintable(QString::number(qAlpha(spread))));
        QVERIFY(under != spread);   // blur first, then a hard-edged glow around the blurred shape
        // the render margin adds up, so nothing of the spread glow is clipped
        VObject o = rectObj();
        o.effects = {glow, blur};
        QVERIFY(o.effectMargin() >= glow.width + blur.softness * 3);
        QVERIFY(o.visualBounds().contains(o.bounds().adjusted(-20, -20, 20, 20)));
    }
    void reorderUpdatesResult() {
        Document *d = Document::createNew(100, 80, Qt::white);
        VObject o = rectObj();
        o.id = d->newId();
        LiveEffect glow = LiveEffect::make(EffectType::OuterGlow);
        glow.color = QColor(255, 0, 0); glow.width = 8; glow.softness = 0;
        LiveEffect hue = LiveEffect::make(EffectType::HueSaturation);
        hue.hue = 120;
        o.effects = {hue, glow};
        d->addObject(o, 0, false);
        d->commit(QStringLiteral("fx"));
        const QRgb before = px(d->render(0, true), 50, 15);
        d->find(o.id)->effects = {glow, hue};
        d->commit(QStringLiteral("reorder"));
        const QRgb after = px(d->render(0, true), 50, 15);
        QVERIFY(qRed(before) > qGreen(before));
        QVERIFY(qGreen(after) > qRed(after));
        delete d;
    }

    // ---- Feather Edges
    void featherEdgesInside() {
        QImage src(60, 40, QImage::Format_ARGB32_Premultiplied);
        src.fill(qRgb(200, 30, 30));
        const QImage soft = fx::featherEdges(src, 10, false, 100), hard = fx::featherEdges(src, 10, false, 0);
        QCOMPARE(soft.size(), src.size());
        auto a = [](const QImage &im, int x, int y) { return qAlpha(px(im, x, y)); };
        QCOMPARE(a(soft, 30, 20), 255);
        QCOMPARE(a(soft, 12, 20), 255);
        QVERIFY(a(soft, 0, 20) < 40);
        QVERIFY(a(soft, 0, 0) < 40);
        for (int x = 1; x <= 10; ++x) QVERIFY(a(soft, x, 20) >= a(soft, x - 1, 20));   // ramps up inward
        QVERIFY(a(hard, 2, 20) > a(soft, 2, 20) + 30);                                 // hard keeps more near the edge
        // the colour is untouched, only the alpha fades
        const QRgb c = qUnpremultiply(soft.pixel(4, 20));
        QVERIFY(qAbs(qRed(c) - 200) <= 3 && qAbs(qGreen(c) - 30) <= 3);
        // through fx::adjust (the Live Effect / filter path) with the default settings
        LiveEffect e = LiveEffect::make(EffectType::FeatherEdges);
        QVERIFY(effectIsAdjustment(e.type));
        QCOMPARE(changed(run(e, src), fx::featherEdges(src, e.width, false, e.amount)), 0);
        // a fully transparent image is left alone
        QImage clear(20, 20, QImage::Format_ARGB32_Premultiplied);
        clear.fill(0);
        QCOMPARE(fx::featherEdges(clear, 5, true, 50), clear);
    }
    void featherEdgesLiveEffect() {
        LiveEffect in = LiveEffect::make(EffectType::FeatherEdges);
        in.width = 8; in.amount = 100;
        LiveEffect cen = in;
        cen.featherCentered = true;
        const QImage none = renderWith({}), inside = renderWith({in}), centred = renderWith({cen});
        // inside: the outline stays put, the edge fades towards the white canvas
        QCOMPARE(px(inside, 50, 40), px(none, 50, 40));
        QVERIFY2(qAlpha(px(inside, 50, 20)) < qAlpha(px(none, 50, 20)) - 80, qPrintable(QString::number(qAlpha(px(inside, 50, 20)))));
        QCOMPARE(qAlpha(px(inside, 50, 18)), 0);
        // centred: the fade spreads 4 px past the edge in the object's own colour
        const QRgb out = px(centred, 50, 18);
        QVERIFY2(qAlpha(out) > 10 && qBlue(qUnpremultiply(out)) > qRed(qUnpremultiply(out)), qPrintable(QColor(out).name()));
        QCOMPARE(qAlpha(px(centred, 50, 12)), 0);
        // in the pipeline: a glow after the feather surrounds the softened shape; before it, the glow is feathered too
        LiveEffect glow = LiveEffect::make(EffectType::OuterGlow);
        glow.color = QColor(255, 0, 0); glow.width = 6; glow.softness = 0;
        QVERIFY(px(renderWith({glow, in}), 50, 16) != px(renderWith({in, glow}), 50, 16));
        // a vector object and a bitmap both feather
        VObject o = rectObj();
        o.effects = {cen};
        QVERIFY(o.visualBounds().contains(o.bounds().adjusted(-4, -4, 4, 4)));
    }

    void lwdRoundTripAllEffects() {
        Document *d = Document::createNew(100, 80, Qt::white);
        VObject o = rectObj();
        o.id = d->newId();
        for (EffectType t : allEffectTypes()) {
            LiveEffect e = LiveEffect::make(t);
            e.enabled = int(t) % 2;
            e.preset = BevelPreset::Inverted;
            e.showObject = false;
            e.amount = 33; e.threshold = 77; e.quality = 9; e.posterize = 6; e.colorNoise = true;
            e.brightness = -12; e.contrast = 25; e.hue = 45; e.saturation = -30; e.lightness = 10; e.colorize = true;
            e.levels[ChBlue] = {10, 240, 5, 250, 1.5};
            e.curves[ChRed] = QPolygonF({QPointF(0, 10), QPointF(100, 150), QPointF(255, 240)});
            e.balance[2][1] = -40; e.preserveLuminosity = false;
            e.blend = BlendMode::Multiply;
            e.angle = 200; e.distance = 12;
            e.keepColor = false; e.offsetX = 5; e.offsetY = -9; e.undefinedAreas = LiveEffect::RepeatEdge;
            e.featherCentered = true;
            o.effects << e;
        }
        d->addObject(o, 0, false);
        d->commit(QStringLiteral("fx"));
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(e);
        const VObject &r = e->objects(0).first();
        QCOMPARE(r.effects.size(), o.effects.size());
        for (int i = 0; i < r.effects.size(); ++i) {
            const LiveEffect &x = o.effects[i], &y = r.effects[i];
            QCOMPARE(y.type, x.type);
            QCOMPARE(y.enabled, x.enabled);
            QCOMPARE(y.angle, x.angle);
            // a rendered comparison covers all the type-specific fields that matter
            QCOMPARE(changed(renderWith({x}), renderWith({y})), 0);
        }
        // spot checks
        auto find = [&](EffectType t) { for (const auto &x : r.effects) if (x.type == t) return x; return LiveEffect(); };
        QCOMPARE(find(EffectType::Levels).levels[ChBlue], (LevelsChannel{10, 240, 5, 250, 1.5}));
        QCOMPARE(find(EffectType::Curves).curves[ChRed].size(), 3);
        QCOMPARE(find(EffectType::ColorBalance).balance[2][1], -40.0);
        QCOMPARE(find(EffectType::InnerBevel).preset, BevelPreset::Inverted);
        QCOMPARE(find(EffectType::Posterize).posterize, 6);
        QCOMPARE(find(EffectType::ZoomBlur).quality, 9);
        QCOMPARE(find(EffectType::ColorFill).blend, BlendMode::Multiply);
        QCOMPARE(find(EffectType::HighPass).keepColor, false);
        QCOMPARE(find(EffectType::Offset).offsetX, 5.0);
        QCOMPARE(find(EffectType::Offset).offsetY, -9.0);
        QCOMPARE(find(EffectType::Offset).undefinedAreas, int(LiveEffect::RepeatEdge));
        QCOMPARE(find(EffectType::FeatherEdges).featherCentered, true);
        QCOMPARE(find(EffectType::FeatherEdges).amount, 33.0);
        delete d;
    }

    // ---- High Pass / Offset / Make Seamless Tile
    void offsetHalfTwiceRestores() {
        QImage src = card().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QVERIFY(src.width() % 2 == 0 && src.height() % 2 == 0);
        LiveEffect e = LiveEffect::make(EffectType::Offset);
        QCOMPARE(e.undefinedAreas, int(LiveEffect::WrapAround)); // the default
        e.offsetX = src.width() / 2; e.offsetY = src.height() / 2;
        QImage once = run(e, src);
        QVERIFY(changed(once, src) > 0);
        QCOMPARE(px(once, src.width() / 2, src.height() / 2), px(src, 0, 0));
        QImage twice = run(e, once);
        QCOMPARE(changed(twice, src), 0); // exact
        // negative offsets and the other modes
        QImage n = fx::offset(src, -3, 0, LiveEffect::WrapAround);
        QCOMPARE(n.pixel(src.width() - 3, 5), src.pixel(0, 5));
        QImage t = fx::offset(src, 4, 0, LiveEffect::Transparent);
        QCOMPARE(qAlpha(t.pixel(2, 5)), 0);
        QCOMPARE(t.pixel(4, 5), src.pixel(0, 5));
        QImage r = fx::offset(src, 4, 0, LiveEffect::RepeatEdge);
        QCOMPARE(r.pixel(1, 5), src.pixel(0, 5));
    }
    // Offset is an exact integer wrap: no frame, no seam, no smoothing, size and opacity unchanged
    static QImage noise(int w, int h, quint32 seed) {
        QImage im(w, h, QImage::Format_ARGB32_Premultiplied);
        QRandomGenerator g(seed);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) im.setPixel(x, y, qRgb(g.bounded(256), g.bounded(256), g.bounded(256)));
        return im;
    }
    static bool exactWrap(const QImage &out, const QImage &src, int dx, int dy, int ox = 0, int oy = 0) {
        const int w = src.width(), h = src.height();
        const QImage O = out.convertToFormat(QImage::Format_ARGB32), S = src.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const QRgb got = O.pixel(ox + x, oy + y);
                const QRgb want = S.pixel((((x - dx) % w) + w) % w, (((y - dy) % h) + h) % h);
                if (qAlpha(got) != 255 || got != want) {
                    qWarning("offset (%d,%d): pixel %d,%d is %08x, want %08x", dx, dy, x, y, got, want);
                    return false;
                }
            }
        return true;
    }
    void offsetIsExactWrap() {
        const QImage src = noise(101, 77, 7);
        const QList<QPoint> offs = {{50, 38}, {13, -7}, {-13, 7}, {-250, 333}, {0, 0}, {101, -77}};
        for (const QPoint o : offs) {
            const QImage out = fx::offset(src, o.x(), o.y(), LiveEffect::WrapAround);
            QCOMPARE(out.size(), src.size());
            QVERIFY(exactWrap(out, src, o.x(), o.y()));
            LiveEffect e = LiveEffect::make(EffectType::Offset);   // the destructive filter path
            e.offsetX = o.x(); e.offsetY = o.y();
            QImage f = src;
            fx::adjust(f, e);
            QCOMPARE(f.size(), src.size());
            QVERIFY(exactWrap(f, src, o.x(), o.y()));
        }
    }
    void offsetLiveEffectHasNoBorder() {
        // a noise bitmap as a Live Effect: wraps inside its own pixels (not the render padding),
        // also when it pokes out past the canvas; checked in the render and in a PNG export
        const QImage src = noise(64, 50, 11);
        for (const QPoint at : {QPoint(10, 20), QPoint(-20, -9), QPoint(0, 0)}) {
            for (const QPoint o : {QPoint(32, 25), QPoint(13, -7), QPoint(-5, 31)}) {
                Document *d = Document::createNew(100, 80, Qt::white);
                VObject b;
                b.type = ObjType::Bitmap;
                b.image = src;
                b.rect = QRectF(at, src.size());
                LiveEffect e = LiveEffect::make(EffectType::Offset);
                e.offsetX = o.x(); e.offsetY = o.y();
                b.effects = {e};
                b.id = d->newId();
                d->objects(0).append(b);
                d->commit(QStringLiteral("t"));
                const QImage frame = d->render(0, true);
                QCOMPARE(frame.size(), QSize(100, 80));
                // wrap the expected image by hand and compare everything on the canvas
                const QImage want = fx::offset(src, o.x(), o.y(), LiveEffect::WrapAround).convertToFormat(QImage::Format_ARGB32);
                const QImage got = frame.convertToFormat(QImage::Format_ARGB32);
                int bad = 0;
                for (int y = 0; y < 80; ++y)
                    for (int x = 0; x < 100; ++x) {
                        const QPoint ip(x - at.x(), y - at.y());
                        const QRgb exp = want.rect().contains(ip) ? want.pixel(ip) : qRgb(255, 255, 255);
                        bad += got.pixel(x, y) != exp;
                    }
                QCOMPARE(bad, 0);
                // the exported file shows the same pixels
                QByteArray png;
                ExportOptions opt;
                opt.format = Fmt::PNG;
                QVERIFY(ImageIO::writeData(png, d->toImageSeq(false), opt));
                ImageSeq back;
                QVERIFY(ImageIO::readData(png, back));
                QCOMPARE(changed(back.frames.first().image, frame), 0);
                delete d;
            }
        }
    }
    void highPassFlatIsMidGrey() {
        for (bool keep : {true, false}) {
            QImage flat(64, 48, QImage::Format_ARGB32_Premultiplied);
            flat.fill(qRgb(200, 30, 90));
            LiveEffect e = LiveEffect::make(EffectType::HighPass);
            e.keepColor = keep;
            for (double r : {0.1, 3.0, 40.0, 250.0}) {
                e.softness = r;
                QImage out = run(e, flat).convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < out.height(); ++y)
                    for (int x = 0; x < out.width(); ++x)
                        if (out.pixel(x, y) != qRgb(128, 128, 128))
                            QFAIL(qPrintable(QStringLiteral("r=%1 keep=%2 (%3,%4) = %5").arg(r).arg(keep).arg(x).arg(y).arg(out.pixel(x, y), 8, 16)));
            }
        }
        // an edge produces values above and below mid-grey; greyscale mode gives grey output
        LiveEffect e = LiveEffect::make(EffectType::HighPass);
        e.keepColor = false;
        QImage out = run(e, card()).convertToFormat(QImage::Format_ARGB32);
        int lo = 0, hi = 0;
        for (int y = 0; y < out.height(); ++y) for (int x = 0; x < out.width(); ++x) {
            QRgb c = out.pixel(x, y);
            QVERIFY(qRed(c) == qGreen(c) && qGreen(c) == qBlue(c));
            lo += qRed(c) < 120; hi += qRed(c) > 136;
        }
        QVERIFY(lo > 0 && hi > 0);
    }
    void seamlessTileWrapsSmoothly() {
        // a horizontal ramp jumps from 255 back to 0 when tiled; the seamless version must not
        QImage ramp(100, 60, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 60; ++y) for (int x = 0; x < 100; ++x) ramp.setPixel(x, y, qRgb(x * 255 / 99, y * 4, 100));
        auto maxJump = [](const QImage &im) {
            int m = 0;
            for (int y = 0; y < im.height(); ++y) m = qMax(m, std::abs(qRed(im.pixel(0, y)) - qRed(im.pixel(im.width() - 1, y))));
            for (int x = 0; x < im.width(); ++x) m = qMax(m, std::abs(qGreen(im.pixel(x, 0)) - qGreen(im.pixel(x, im.height() - 1))));
            return m;
        };
        QVERIFY(maxJump(ramp) > 200);
        QImage t = fx::makeSeamless(ramp, 30);
        QCOMPARE(t.size(), ramp.size());
        QVERIFY2(maxJump(t) <= 6, qPrintable(QString::number(maxJump(t))));
        // and the inner seam (where the old edges now meet) is cross-faded
        int seam = 0;
        for (int y = 0; y < 60; ++y) seam = qMax(seam, std::abs(qRed(t.pixel(49, y)) - qRed(t.pixel(50, y))));
        QVERIFY2(seam <= 12, qPrintable(QString::number(seam)));
    }

    // ---- retouch tools
    void retouchDabs() {
        QImage im = card();
        QImage b = im;
        retouch::blurDab(b, QPointF(21, 24), 12, 100);
        QVERIFY(changed(b, im) > 0);
        QCOMPARE(px(b, 60, 4), px(im, 60, 4)); // outside the dab
        QImage s = im;
        retouch::sharpenDab(s, QPointF(21, 24), 12, 100);
        QVERIFY(changed(s, im) > 0);
        QImage dd = im, bb = im;
        retouch::dodgeBurnDab(dd, QPointF(32, 8), 10, 100, retouch::Range::Midtones, false);
        retouch::dodgeBurnDab(bb, QPointF(32, 8), 10, 100, retouch::Range::Midtones, true);
        QVERIFY(qGray(px(dd, 32, 8)) > qGray(px(im, 32, 8)));
        QVERIFY(qGray(px(bb, 32, 8)) < qGray(px(im, 32, 8)));
        // rubber stamp clones from the offset
        QImage st = im;
        retouch::stampDab(st, im, QPointF(10, 40), QPointF(0, -36), 6, 100); // copy from (10,4)
        QCOMPARE(px(st, 10, 40), px(im, 10, 4));
        // smudge drags colour
        QImage sm = im;
        retouch::Smudger sg;
        sg.start(sm, QPointF(10, 24), 8);
        for (const QPointF &p : retouch::dabsAlong(QPointF(10, 24), QPointF(30, 24), 2)) sg.dab(sm, p, 80);
        QVERIFY(qRed(px(sm, 24, 24)) > qRed(px(im, 24, 24)) + 20); // red dragged into the green patch
        // clip limits the effect
        QImage clip(im.size(), QImage::Format_Alpha8);
        clip.fill(0);
        QImage c = im;
        retouch::dodgeBurnDab(c, QPointF(32, 8), 10, 100, retouch::Range::Midtones, false, &clip);
        QCOMPARE(changed(c, im), 0);
        // dab spacing
        QCOMPARE(retouch::dabsAlong(QPointF(0, 0), QPointF(10, 0), 2).size(), 5);
    }
    // healing: the texture comes from the source, the colour from around the destination
    void brushEngine() {
        auto stroke = [](const brush::Params &p, QImage img, QPointF a, QPointF b) {
            brush::Stroke s;
            s.begin(img, p);
            s.composite(img, s.segment(a, a));
            s.composite(img, s.segment(a, b));
            s.composite(img, s.finish());
            return img;
        };
        QImage blank(100, 60, QImage::Format_ARGB32_Premultiplied);
        blank.fill(Qt::transparent);
        brush::Params p;
        p.size = 6;
        p.color = Qt::red;
        QImage r = stroke(p, blank, {20, 30}, {60, 30});
        QCOMPARE(r.pixel(40, 30), qRgba(255, 0, 0, 255));
        QCOMPARE(qAlpha(r.pixel(40, 38)), 0);
        // opacity applies to the stroke: going back over it does not build up
        p.opacity = 50;
        {
            brush::Stroke s;
            QImage img = blank;
            s.begin(img, p);
            s.composite(img, s.segment({20, 30}, {20, 30}));
            s.composite(img, s.segment({20, 30}, {60, 30}));
            s.composite(img, s.segment({60, 30}, {20, 30}));
            s.composite(img, s.finish());
            QVERIFY(qAbs(qAlpha(img.pixel(40, 30)) - 128) <= 1);
        }
        p.opacity = 100;
        // soft edge: partial coverage towards the rim; square tip covers the corners
        p.size = 20;
        p.edge = 100;
        QImage soft = stroke(p, blank, {50, 30}, {50, 30});
        QCOMPARE(qAlpha(soft.pixel(50, 30)), 255);
        QVERIFY(qAlpha(soft.pixel(50, 37)) > 0 && qAlpha(soft.pixel(50, 37)) < 200);
        p.edge = 0;
        p.square = true;
        QImage sq = stroke(p, blank, {50, 30}, {50, 30});
        QCOMPARE(qAlpha(sq.pixel(58, 38)), 255);    // inside the square's corner
        p.square = false;
        QImage rd = stroke(p, blank, {50, 30}, {50, 30});
        QCOMPARE(qAlpha(rd.pixel(58, 38)), 0);      // outside the circle
        // eraser removes, preserve transparency keeps the alpha
        QImage half(100, 60, QImage::Format_ARGB32_Premultiplied);
        half.fill(Qt::transparent);
        { QPainter hp(&half); hp.fillRect(0, 0, 50, 60, Qt::blue); }
        brush::Params e = p;
        e.erase = true;
        QImage er = stroke(e, half, {10, 30}, {40, 30});
        QCOMPARE(qAlpha(er.pixel(25, 30)), 0);
        QCOMPARE(qAlpha(er.pixel(25, 50)), 255);
        brush::Params pt = p;
        pt.preserveTransparency = true;
        QImage ptr = stroke(pt, half, {30, 30}, {70, 30});
        QCOMPARE(ptr.pixel(40, 30), qRgba(255, 0, 0, 255));
        QCOMPARE(qAlpha(ptr.pixel(60, 30)), 0);     // was transparent: stays so
        // blend mode: Multiply red over blue gives black
        brush::Params mul = p;
        mul.blend = BlendMode::Multiply;
        QImage m = stroke(mul, half, {30, 30}, {30, 30});
        QCOMPARE(m.pixel(30, 30), qRgba(0, 0, 0, 255));
        // pencil: aliased 1px line, exact pixels
        brush::Params pen;
        pen.pencil = true;
        pen.antialias = false;
        pen.size = 1;
        pen.color = Qt::green;
        QImage pl = stroke(pen, blank, {10.5, 10.5}, {20.5, 10.5});
        QCOMPARE(pl.pixel(15, 10), qRgba(0, 255, 0, 255));
        QCOMPARE(qAlpha(pl.pixel(15, 11)), 0);
        QCOMPARE(qAlpha(pl.pixel(15, 9)), 0);
        // texture lightens part of the stroke
        brush::Params tx = p;
        tx.texture = QStringLiteral("Grid");
        tx.textureAmount = 100;
        QImage t = stroke(tx, blank, {10, 30}, {90, 30});
        int lo = 255, hi = 0;
        for (int x = 20; x < 80; ++x) { lo = qMin(lo, qAlpha(t.pixel(x, 30))); hi = qMax(hi, qAlpha(t.pixel(x, 30))); }
        QVERIFY(lo < 50 && hi == 255);
        // air brush builds up as dabs pile on
        brush::Params ab = p;
        ab.category = brush::Category::AirBrush;
        brush::Stroke s;
        QImage air = blank;
        s.begin(air, ab);
        s.composite(air, s.segment({50, 30}, {50, 30}));
        const int a1 = qAlpha(air.pixel(50, 30));
        for (int i = 0; i < 10; ++i) s.composite(air, s.segment({50, 30}, {50.5, 30}));
        QVERIFY(qAlpha(air.pixel(50, 30)) > a1);
        // Basic..Libreworks (Fireworks categories plus the Libreworks set; approved for 0.3.20)
        QCOMPARE(brush::categoryNames().size(), int(brush::Category::Libreworks) + 1);
        // paint bucket fill of a mask
        QImage fm = blank;
        QImage mask(fm.size(), QImage::Format_Alpha8);
        mask.fill(0);
        { QPainter mp(&mask); mp.fillRect(10, 10, 20, 20, QColor(0, 0, 0, 255)); }
        brush::fillMask(fm, mask, Qt::red, 50, BlendMode::Normal, QString(), 0);
        QVERIFY(qAbs(qAlpha(fm.pixel(20, 20)) - 128) <= 1);
        QCOMPARE(qAlpha(fm.pixel(40, 20)), 0);
    }

    void healingBlend() {
        QImage im(160, 80, QImage::Format_ARGB32_Premultiplied);
        quint32 seed = 7;
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 160; ++x) {
                if (x < 80) {
                    seed = seed * 1664525u + 1013904223u;
                    const int g = 90 + int((seed >> 24) % 60);   // noisy grey around 120
                    im.setPixel(x, y, qRgb(g, g, g));
                } else im.setPixel(x, y, qRgb(200, 90, 50));      // flat orange
            }
        auto stats = [](const QImage &img, int cx, int cy, int r, double mean[3], double &var) {
            double s[3] = {0, 0, 0}; int n = 0;
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x) { const QRgb p = img.pixel(x, y); s[0] += qRed(p); s[1] += qGreen(p); s[2] += qBlue(p); ++n; }
            for (int i = 0; i < 3; ++i) mean[i] = s[i] / n;
            var = 0;
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x) var += std::pow(qGreen(img.pixel(x, y)) - mean[1], 2);
            var /= n;
        };
        QImage h = im;
        const QImage src = im;
        retouch::healDab(h, src, QPointF(120, 40), QPointF(-80, 0), 24, 50, retouch::HealMode::Normal);
        double m[3], var;
        stats(h, 120, 40, 6, m, var);
        QVERIFY2(std::abs(m[0] - 200) < 10 && std::abs(m[1] - 90) < 10 && std::abs(m[2] - 50) < 10,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(m[0]).arg(m[1]).arg(m[2])));
        QVERIFY2(var > 60, qPrintable(QString::number(var)));   // was 0: the texture came in
        QCOMPARE(px(h, 150, 40), px(im, 150, 40));               // outside the dab
        // a plain stamp would have brought the grey too
        QImage st = im;
        retouch::stampDab(st, src, QPointF(120, 40), QPointF(-80, 0), 24, 100);
        stats(st, 120, 40, 6, m, var);
        QVERIFY(std::abs(m[0] - 200) > 50);
        // Replace heals with a hard tip: full strength right up to the edge, nothing past it
        QImage rp = im;
        retouch::healDab(rp, src, QPointF(120, 40), QPointF(-80, 0), 24, 50, retouch::HealMode::Replace);
        stats(rp, 120, 40, 6, m, var);
        QVERIFY(std::abs(m[0] - 200) < 10 && var > 60);
        QVERIFY(px(rp, 130, 40) != px(im, 130, 40) && px(rp, 130, 40) != px(h, 130, 40));
        QCOMPARE(px(rp, 133, 40), px(im, 133, 40));
        // Darken only darkens
        QImage dk = im;
        retouch::healDab(dk, src, QPointF(120, 40), QPointF(-80, 0), 24, 50, retouch::HealMode::Darken);
        for (int x = 110; x < 130; ++x) QVERIFY(qGray(px(dk, x, 40)) <= qGray(px(im, x, 40)));
        // a clip limits it
        QImage clip(im.size(), QImage::Format_Alpha8);
        clip.fill(0);
        QImage c = im;
        retouch::healDab(c, src, QPointF(120, 40), QPointF(-80, 0), 24, 50, retouch::HealMode::Normal, &clip);
        QCOMPARE(changed(c, im), 0);

        // spot healing: a blemish on a textured patch disappears without a source
        QImage sp = im;
        {
            QPainter p(&sp);
            p.fillRect(QRect(37, 37, 6, 6), Qt::red);
        }
        retouch::spotHealDab(sp, QPointF(40, 40), 16, 50, retouch::HealMode::Normal);
        stats(sp, 40, 40, 2, m, var);
        QVERIFY2(std::abs(m[0] - m[1]) < 25 && std::abs(m[1] - 120) < 30,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(m[0]).arg(m[1]).arg(m[2])));
        QVERIFY(var > 10);   // textured, not a smear
        // and on the flat area it takes the flat colour
        QImage sf = im;
        {
            QPainter p(&sf);
            p.fillRect(QRect(117, 37, 6, 6), Qt::black);
        }
        retouch::spotHealDab(sf, QPointF(120, 40), 16, 50, retouch::HealMode::Normal);
        QVERIFY(std::abs(qRed(px(sf, 120, 40)) - 200) < 12 && std::abs(qGreen(px(sf, 120, 40)) - 90) < 12);
    }
};

QTEST_MAIN(EffectsTest)
#include "effects_test.moc"
