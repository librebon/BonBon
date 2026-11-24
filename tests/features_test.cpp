#include "Brand.h"
// Feature tests for the core (rendering, document features, file format).
// Run with QT_QPA_PLATFORM=offscreen (ctest sets this).
#include "core/Document.h"
#include "core/Render.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "io/DocIO.h"
#include "io/HtmlExport.h"
#include <QTemporaryDir>
#include "core/CorePrefs.h"
#include <memory>
#include <QImageReader>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtTest>
#include <memory>
#include "io/Zip.h"
#include "core/Styles.h"
#include "core/Paths.h"
#include "core/Fills.h"
#include <QPainter>

namespace {
VObject makeText(TextAA aa, bool stroke = false) {
    VObject o;
    o.id = 1;
    o.type = ObjType::Text;
    o.text = QStringLiteral("Libreworks AaGg 123\nSecond line");
    o.fontSize = 26;
    o.fillOn = true;
    o.fill = Qt::black;
    o.strokeOn = stroke;
    o.stroke = QColor(200, 0, 0);
    o.strokeWidth = 1;
    o.textAA = aa;
    o.rect = QRectF(4, 4, 10, 10);
    o.fitTextBox();
    return o;
}
// distinct alpha values of an image
QSet<int> alphas(const QImage &im0) {
    QImage im = im0.convertToFormat(QImage::Format_ARGB32);
    QSet<int> s;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) s.insert(qAlpha(im.pixel(x, y)));
    return s;
}
// distinct grey levels of an image composited onto white
QSet<int> greys(const QImage &im0) {
    QImage im = im0.convertToFormat(QImage::Format_ARGB32);
    QSet<int> s;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) s.insert(qGray(im.pixel(x, y)));
    return s;
}
int inkPixels(const QImage &im0) {
    QImage im = im0.convertToFormat(QImage::Format_ARGB32);
    int n = 0;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) n += qAlpha(im.pixel(x, y)) > 0;
    return n;
}
Document *docWith(const VObject &o) {
    Document *d = Document::createNew(320, 120, Qt::white);
    VObject c = o;
    c.id = d->newId();
    d->addObject(c, 0, false);
    d->commit(QStringLiteral("add"));
    return d;
}
Document *webDoc() {
    Document *d = Document::createNew(400, 300, Qt::white);
    VObject r; r.type = ObjType::Rect; r.rect = QRectF(20, 20, 200, 100); r.fillOn = true; r.fill = Qt::red; r.id = d->newId();
    d->addObject(r, 0, false);
    d->addFrame(true);           // frame 2 for the rollover state
    d->curFrame = 1;
    for (auto &o : d->s.frames[1].layers[0]) if (o.type == ObjType::Rect) o.fill = Qt::blue;
    d->curFrame = 0;
    auto web = [d](ObjType t, QRectF rect, const QString &name) {
        VObject o; o.type = t; o.rect = rect; o.name = name; o.id = d->newId();
        d->addObject(o, -1, false);
        return o.id;
    };
    quint64 a = web(ObjType::Slice, QRectF(20, 20, 200, 100), QStringLiteral("banner"));
    quint64 b = web(ObjType::Slice, QRectF(250, 150, 100, 80), QStringLiteral("button"));
    web(ObjType::Hotspot, QRectF(30, 200, 60, 60), QStringLiteral("hs"));
    d->commit(QStringLiteral("web"));
    VObject *A = d->find(a);
    A->link = QStringLiteral("https://example.com/?a=1&b=2");
    A->alt = QStringLiteral("Banner");
    A->target = QStringLiteral("_blank");
    A->behavior = WebBehavior::SimpleRollover;
    VObject *B = d->find(b);
    B->sliceCustomOpt = true;
    B->sliceOpt.format = Fmt::JPEG;
    B->sliceOpt.quality = 60;
    B->behavior = WebBehavior::SwapImage;
    B->swapTarget = a;
    B->swapFrame = 1;
    int wl = d->webLayer();
    for (auto &o : d->s.frames[0].layers[wl]) if (o.type == ObjType::Hotspot) { o.webShape = WebShape::Circle; o.link = QStringLiteral("/circle"); }
    VObject poly; poly.type = ObjType::Hotspot; poly.webShape = WebShape::Polygon; poly.closed = true;
    poly.points = {QPointF(300, 20), QPointF(380, 40), QPointF(340, 100)}; poly.rect = poly.points.boundingRect(); poly.name = QStringLiteral("tri"); poly.id = d->newId();
    poly.link = QStringLiteral("/tri");
    d->addObject(poly, -1, false);
    d->commit(QStringLiteral("web props"));
    return d;
}
}

class Features : public QObject {
    Q_OBJECT
private slots:
    void gifLossAndDitherAmount() {
        // a noisy gradient: Loss must shrink the file, dither amount 0 must equal "no dither"
        QImage img(160, 120, QImage::Format_ARGB32);
        quint32 seed = 7;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                const int n = int(seed >> 28);
                img.setPixel(x, y, qRgb(qBound(0, x + n, 255), qBound(0, y * 2 + n, 255), 120 + n));
            }
        ImageSeq seq;
        seq.width = img.width(); seq.height = img.height();
        seq.frames.append({img, 100});
        auto enc = [&](bool dither, int amount, int loss) {
            ExportOptions o; o.format = Fmt::GIF; o.colors = 64; o.dither = dither; o.ditherAmount = amount; o.loss = loss;
            QByteArray b; [&] { QVERIFY(ImageIO::writeData(b, seq, o)); }();
            return b;
        };
        const QByteArray l0 = enc(true, 100, 0), l80 = enc(true, 100, 80);
        QVERIFY2(l80.size() < l0.size() * 9 / 10, qPrintable(QStringLiteral("%1 vs %2").arg(l80.size()).arg(l0.size())));
        QCOMPARE(enc(true, 0, 0), enc(false, 100, 0));
        QVERIFY(enc(true, 50, 0) != enc(true, 100, 0));
        ImageSeq back;
        QVERIFY(ImageIO::readData(l80, back));
        QCOMPARE(back.frames.first().image.size(), img.size());
    }
    void textAntiAliasObject_data() {
        QTest::addColumn<bool>("stroke");
        QTest::newRow("glyph fill (drawText)") << false;
        QTest::newRow("outlined (path fill)") << true;
    }
    void textAntiAliasObject() {
        QFETCH(bool, stroke);
        const QRect clip(0, 0, 1000, 1000);
        QRect area;
        QImage off = render::renderObject(makeText(TextAA::None, stroke), &area, clip);
        QSet<int> a = alphas(off);
        qInfo() << "No Anti-Alias distinct alpha values:" << a.size();
        QVERIFY2(a.size() == 2 && a.contains(0) && a.contains(255), "AA off must give only fully opaque / fully transparent pixels");
        for (TextAA m : {TextAA::Crisp, TextAA::Strong, TextAA::Smooth, TextAA::System}) {
            QImage on = render::renderObject(makeText(m, stroke), &area, clip);
            const int n = alphas(on).size();
            qInfo() << textAAKey(m) << "distinct alpha values:" << n << "ink" << inkPixels(on);
            QVERIFY2(n > 8, qPrintable(QStringLiteral("%1 must anti-alias").arg(textAAKey(m))));
        }
    }
    void textAntiAliasModesDiffer() {
        const QRect clip(0, 0, 1000, 1000);
        QRect area;
        QImage smooth = render::renderObject(makeText(TextAA::Smooth), &area, clip);
        QImage strong = render::renderObject(makeText(TextAA::Strong), &area, clip);
        QVERIFY(inkPixels(strong) > inkPixels(smooth)); // Strong emboldens
        QImage crisp = render::renderObject(makeText(TextAA::Crisp), &area, clip);
        QVERIFY(crisp != smooth || crisp != strong);
    }
    void textAntiAliasExport() {
        // full frame render (what the canvas cache and every exporter use) + PNG round trip
        for (TextAA m : {TextAA::None, TextAA::Smooth}) {
            std::unique_ptr<Document> d(docWith(makeText(m)));
            QImage frame = d->render(0, true);
            ExportOptions eo;
            eo.format = Fmt::PNG;
            QByteArray png;
            QVERIFY(ImageIO::writeData(png, d->toImageSeq(false), eo));
            ImageSeq back;
            QVERIFY(ImageIO::readData(png, back));
            for (const QImage &im : {frame, back.frames.first().image}) {
                const int g = greys(im).size();
                if (m == TextAA::None) QCOMPARE(g, 2);  // black text on white only
                else QVERIFY(g > 8);
            }
        }
    }
    void textAntiAliasLwd() {
        std::unique_ptr<Document> d(docWith(makeText(TextAA::Strong)));
        QByteArray data = lwd::serialize(*d);
        std::unique_ptr<Document> e(lwd::deserialize(data));
        QVERIFY(e);
        QCOMPARE(e->objects(0).last().textAA, TextAA::Strong);
        // v0.1 files: only the bool "antialias" exists
        QMap<QString, QByteArray> files;
        QVERIFY(zip::read(data, files));
        QJsonObject root = QJsonDocument::fromJson(files.value(QStringLiteral("document.json"))).object();
        QJsonArray frames = root["frames"].toArray();
        QJsonObject f0 = frames[0].toObject();
        QJsonArray layers = f0["layers"].toArray();
        QJsonArray objs = layers[0].toArray();
        QJsonObject t = objs.last().toObject();
        t.remove("textAntialias");
        t["antialias"] = false;
        objs[objs.size() - 1] = t; layers[0] = objs; f0["layers"] = layers; frames[0] = f0; root["frames"] = frames;
        files[QStringLiteral("document.json")] = QJsonDocument(root).toJson();
        QVector<zip::Entry> entries;
        entries.append({QStringLiteral("mimetype"), files.value(QStringLiteral("mimetype")), false});
        for (auto it = files.begin(); it != files.end(); ++it)
            if (it.key() != QLatin1String("mimetype")) entries.append({it.key(), it.value(), true});
        std::unique_ptr<Document> old(lwd::deserialize(zip::write(entries)));
        QVERIFY(old);
        QCOMPARE(old->objects(0).last().textAA, TextAA::None);
    }
    void webLayerBasics() {
        std::unique_ptr<Document> d(webDoc());
        const int wl = d->webLayer();
        QVERIFY(wl >= 0);
        QVERIFY(d->s.layers[wl].web && d->s.layers[wl].shared);
        QCOMPARE(wl, int(d->s.layers.size()) - 1);              // topmost
        // shared across frames, same ids
        QCOMPARE(d->s.frames[0].layers[wl].size(), d->s.frames[1].layers[wl].size());
        for (int i = 0; i < d->s.frames[0].layers[wl].size(); ++i)
            QCOMPARE(d->s.frames[0].layers[wl][i].id, d->s.frames[1].layers[wl][i].id);
        // web objects never render
        QImage withWeb = d->render(0, true);
        Document *plain = Document::createNew(400, 300, Qt::white);
        VObject r; r.type = ObjType::Rect; r.rect = QRectF(20, 20, 200, 100); r.fillOn = true; r.fill = Qt::red; r.id = plain->newId();
        plain->addObject(r, 0, false);
        plain->commit(QStringLiteral("x"));
        QCOMPARE(withWeb, plain->render(0, true));
        delete plain;
        // new artwork never lands on the web layer even when it is current
        d->curLayer = wl;
        QVERIFY(d->drawingLayer() != wl);
        VObject e; e.type = ObjType::Ellipse; e.rect = QRectF(0, 0, 10, 10); e.id = d->newId();
        d->addObject(e);
        QVERIFY(d->find(e.id));
        for (const VObject &o : d->objects(wl)) QVERIFY(o.isWeb());
    }
    void sharedLayer() {
        std::unique_ptr<Document> d(Document::createNew(100, 100, Qt::white));
        d->addFrame(false);
        d->addFrame(false);
        d->curFrame = 0;
        d->setLayerShared(0, true);
        VObject r; r.type = ObjType::Rect; r.rect = QRectF(5, 5, 20, 20); r.id = d->newId();
        d->addObject(r, 0, false);
        d->commit(QStringLiteral("add"));
        for (int f = 0; f < 3; ++f) QCOMPARE(d->s.frames[f].layers[0].size(), 1);
        d->curFrame = 2;
        d->find(r.id)->rect = QRectF(50, 50, 20, 20);
        d->commit(QStringLiteral("move"));
        QCOMPARE(d->s.frames[0].layers[0][0].rect, QRectF(50, 50, 20, 20));
    }
    void htmlPlan() {
        std::unique_ptr<Document> d(webDoc());
        html::Plan p = html::plan(*d, QStringLiteral("page"));
        QCOMPARE(p.slices.size(), 2);
        qint64 area = 0;
        QImage cover(400, 300, QImage::Format_Grayscale8);
        cover.fill(0);
        QSet<QString> names;
        for (const auto &g : p.regions) {
            area += qint64(g.rect.width()) * g.rect.height();
            QVERIFY(!names.contains(g.name));
            names.insert(g.name);
            for (int y = g.rect.top(); y <= g.rect.bottom(); ++y)
                for (int x = g.rect.left(); x <= g.rect.right(); ++x) {
                    QCOMPARE(qGray(cover.pixel(x, y)), 0);
                    cover.setPixel(x, y, 255);
                }
        }
        QCOMPARE(area, qint64(400) * 300);
        bool found = false;
        for (const auto &g : p.regions) if (g.slice >= 0 && g.rect == QRect(20, 20, 200, 100)) found = true;
        QVERIFY(found);
    }
    void htmlExport_data() {
        QTest::addColumn<int>("layout");
        QTest::newRow("table") << int(html::Layout::Table);
        QTest::newRow("css") << int(html::Layout::Css);
    }
    void htmlExport() {
        QFETCH(int, layout);
        std::unique_ptr<Document> d(webDoc());
        QTemporaryDir tmp;
        html::Options o;
        o.layout = html::Layout(layout);
        QStringList written;
        QString err;
        const QString path = tmp.filePath(QStringLiteral("index.html"));
        QVERIFY2(html::exportHtml(*d, path, o, &err, &written), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString h = QString::fromUtf8(f.readAll());
        QVERIFY(h.contains(QStringLiteral("usemap=\"#map_")));
        QVERIFY(h.contains(QStringLiteral("<area shape=\"circle\"")));
        QVERIFY(h.contains(QStringLiteral("<area shape=\"poly\"")));
        QVERIFY(h.contains(QStringLiteral("href=\"/circle\"")));
        QVERIFY(h.contains(QStringLiteral("href=\"https://example.com/?a=1&amp;b=2\"")));
        QVERIFY(h.contains(QStringLiteral("target=\"_blank\"")));
        QVERIFY(h.contains(QStringLiteral("alt=\"Banner\"")));
        QVERIFY(h.contains(QStringLiteral("onmouseover=\"lwSwap(")));
        QVERIFY(h.contains(QStringLiteral("function lwSwap")));
        if (o.layout == html::Layout::Table) QVERIFY(h.contains(QStringLiteral("<table")));
        else { QVERIFY(h.contains(QStringLiteral("position:absolute"))); QVERIFY(!h.contains(QStringLiteral("<table"))); }
        // images: sizes and per-slice formats
        bool sawJpeg = false, sawBanner = false, sawOver = false;
        qint64 area = 0;
        QVERIFY(written.contains(path));
        for (const QString &w : written) {
            QVERIFY(QFileInfo::exists(w));
            if (w == path) continue;
            QVERIFY2(w.contains(QStringLiteral("/images/")), qPrintable(w));
            QImageReader r(w);
            const QSize sz = r.size();
            QVERIFY(sz.isValid());
            const QString n = QFileInfo(w).completeBaseName();
            if (n.contains(QStringLiteral("_f"))) { sawOver = true; continue; }
            area += qint64(sz.width()) * sz.height();
            if (w.endsWith(QStringLiteral(".jpg"))) { sawJpeg = true; QCOMPARE(sz, QSize(100, 80)); }
            if (n == QStringLiteral("banner")) { sawBanner = true; QCOMPARE(sz, QSize(200, 100)); }
            QVERIFY2(h.contains(QStringLiteral("images/") + QFileInfo(w).fileName()), qPrintable(w));
        }
        QVERIFY(sawJpeg);
        QVERIFY(sawBanner);
        QVERIFY(sawOver);
        QCOMPARE(area, qint64(400) * 300);
        // the rollover image is frame 2 (blue)
        for (const QString &w : written)
            if (QFileInfo(w).completeBaseName() == QStringLiteral("banner_f2")) {
                QImage im(w);
                QCOMPARE(QColor(im.pixel(100, 50)).blue(), 255);
                QCOMPARE(QColor(im.pixel(100, 50)).red(), 0);
            }
    }
    void webLwdRoundTrip() {
        std::unique_ptr<Document> d(webDoc());
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(e);
        const int wl = e->webLayer();
        QVERIFY(wl >= 0);
        QVERIFY(e->s.layers[wl].shared);
        const auto &a = d->objects(d->webLayer());
        const auto &b = e->objects(wl);
        QCOMPARE(b.size(), a.size());
        for (int i = 0; i < a.size(); ++i) {
            QCOMPARE(b[i].type, a[i].type);
            QCOMPARE(b[i].name, a[i].name);
            QCOMPARE(b[i].link, a[i].link);
            QCOMPARE(b[i].alt, a[i].alt);
            QCOMPARE(b[i].target, a[i].target);
            QCOMPARE(b[i].behavior, a[i].behavior);
            QCOMPARE(b[i].swapTarget, a[i].swapTarget);
            QCOMPARE(b[i].swapFrame, a[i].swapFrame);
            QCOMPARE(b[i].webShape, a[i].webShape);
            QCOMPARE(b[i].sliceCustomOpt, a[i].sliceCustomOpt);
            if (a[i].sliceCustomOpt) QCOMPARE(b[i].sliceOpt, a[i].sliceOpt);
            QCOMPARE(b[i].points, a[i].points);
            QCOMPARE(b[i].rect, a[i].rect);
        }
        QCOMPARE(e->s.frames[1].layers[wl].size(), a.size());
    }
    void stylesBuiltins() {
        const auto b = styles::builtins();
        QVERIFY(b.size() >= 20);
        QSet<QString> names;
        for (const Style &st : b) {
            QVERIFY(st.builtin);
            QVERIFY(!st.name.isEmpty());
            names.insert(st.name);
            const QImage p = styles::preview(st, 44);
            QCOMPARE(p.size(), QSize(44, 44));
        }
        QCOMPARE(names.size(), b.size());
    }
    void stylesApply() {
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(0, 0, 50, 50);
        VObject src = r;
        src.fill = QColor(Qt::red);
        src.stroke = QColor(Qt::green);
        src.strokeWidth = 5;
        src.effects << LiveEffect::make(EffectType::DropShadow);
        Style st = Style::fromObject(src, QStringLiteral("Mine"));
        VObject a = r;
        QVERIFY(st.applyTo(a));
        QCOMPARE(a.fill, QColor(Qt::red));
        QCOMPARE(a.stroke, QColor(Qt::green));
        QCOMPARE(a.strokeWidth, 5.0);
        QCOMPARE(a.effects.size(), 1);
        QCOMPARE(a.rect, r.rect);          // geometry untouched
        QVERIFY(!st.applyTo(a));           // idempotent: nothing changes the second time
        // only effects
        st.hasFill = st.hasStroke = false;
        VObject e = r;
        QVERIFY(st.applyTo(e));
        QCOMPARE(e.fill, r.fill);
        QCOMPARE(e.effects.size(), 1);
        // bitmaps get the effects but not fill/stroke; web objects are untouched
        VObject bm;
        bm.type = ObjType::Bitmap;
        bm.rect = QRectF(0, 0, 10, 10);
        Style full = Style::fromObject(src, QStringLiteral("x"));
        QVERIFY(full.applyTo(bm));
        QCOMPARE(bm.effects.size(), 1);
        VObject sl;
        sl.type = ObjType::Slice;
        QVERIFY(!full.applyTo(sl));
        // text style
        VObject t;
        t.type = ObjType::Text;
        t.text = QStringLiteral("Hi");
        t.fontSize = 12;
        VObject ts = t;
        ts.fontSize = 40;
        ts.bold = true;
        Style tst = Style::fromObject(ts, QStringLiteral("T"));
        tst.hasText = true;
        QVERIFY(tst.applyTo(t));
        QCOMPARE(t.fontSize, 40.0);
        QVERIFY(t.bold);
        QCOMPARE(t.text, QStringLiteral("Hi"));
        // a text style does not change the font of a rectangle
        VObject r2 = r;
        tst.applyTo(r2);
        QCOMPARE(r2.fontSize, r.fontSize);
    }
    void stylesJsonAndUserFile() {
        QTemporaryDir td;
        qputenv("XDG_CONFIG_HOME", td.path().toUtf8());
        QCOMPARE(styles::userFile(), td.path() + QLatin1Char('/') + brand::nameLower() + QStringLiteral("/styles.json"));
        QVERIFY(styles::loadUser().isEmpty());
        QVector<Style> all = styles::builtins();
        VObject src;
        src.type = ObjType::Ellipse;
        src.fill = QColor(1, 2, 3);
        LiveEffect g = LiveEffect::make(EffectType::OuterGlow);
        src.effects << g;
        Style u = Style::fromObject(src, QStringLiteral("User One"));
        u.hasStroke = false;
        all << u;
        QString err;
        QVERIFY2(styles::saveUser(all, &err), qPrintable(err)); // only non-builtins are written
        const auto back = styles::loadUser(&err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].name, QStringLiteral("User One"));
        QVERIFY(!back[0].builtin);
        QVERIFY(!back[0].hasStroke);
        QCOMPARE(back[0].attrs.fill, QColor(1, 2, 3));
        QCOMPARE(back[0].attrs.effects.size(), 1);
        QCOMPARE(int(back[0].attrs.effects[0].type), int(EffectType::OuterGlow));
        // JSON roundtrip of every builtin
        for (const Style &st : styles::builtins()) {
            Style r;
            QVERIFY(styles::fromJson(styles::toJson(st), r));
            QCOMPARE(r.name, st.name);
            QCOMPARE(r.hasText, st.hasText);
            QCOMPARE(r.attrs.effects.size(), st.attrs.effects.size());
            VObject a, b;
            a.type = b.type = ObjType::Rect;
            st.applyTo(a);
            r.applyTo(b);
            QCOMPARE(render::renderObject(a, nullptr, QRect(-200, -200, 400, 400)).isNull(), false);
            QCOMPARE(a.fill, b.fill);
            QCOMPARE(a.strokeWidth, b.strokeWidth);
        }
        // corrupt file reports an error, doesn't crash
        QFile f(styles::userFile());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{nope");
        f.close();
        err.clear();
        QVERIFY(styles::loadUser(&err).isEmpty());
        QVERIFY(!err.isEmpty());
        qunsetenv("XDG_CONFIG_HOME");
    }

    // ---------------------------------------------------------------- symbols
    static VObject mkRect(Document *d, const QRectF &r, const QColor &c) {
        VObject o;
        o.type = ObjType::Rect;
        o.rect = r;
        o.fill = c;
        o.strokeOn = false;
        o.antialias = false;
        o.id = d->newId();
        return o;
    }
    void symbolsConvertAndRender() {
        std::unique_ptr<Document> d(Document::createNew(200, 100, Qt::white));
        VObject a = mkRect(d.get(), QRectF(10, 10, 20, 20), Qt::red);
        VObject b = mkRect(d.get(), QRectF(40, 10, 20, 20), Qt::blue);
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->commit(QStringLiteral("rects"));
        const QImage before = d->render(0, true);
        d->setSelection({a.id, b.id});
        const quint64 sid = d->convertToSymbol(QStringLiteral("Pair"), SymbolKind::Graphic);
        QVERIFY(sid);
        d->commit(QStringLiteral("Convert to Symbol"));
        QCOMPARE(d->s.symbols.size(), 1);
        QCOMPARE(d->objects(0).size(), 1);
        const VObject &inst = d->objects(0)[0];
        QVERIFY(inst.isInstance());
        QCOMPARE(inst.symbolId, sid);
        QCOMPARE(inst.rect, QRectF(10, 10, 50, 20));
        QVERIFY(d->selection.contains(inst.id));
        QCOMPARE(d->instanceCount(sid), 1);
        // the instance renders exactly like the originals
        QCOMPARE(d->render(0, true), before);
        // insert a second instance and move it; editing the symbol updates both
        auto ids = d->insertInstance(sid, QPointF(135, 70));
        QCOMPARE(ids.size(), 1);
        QCOMPARE(d->find(ids[0])->rect, QRectF(110, 60, 50, 20));
        QCOMPARE(d->render(0, true).pixelColor(115, 65), QColor(Qt::red));
        auto frames = d->symbol(sid)->frames;
        frames[0][0].fill = QColor(Qt::green);
        d->setSymbolFrames(sid, frames);
        d->commit(QStringLiteral("Edit Symbol"));
        QImage img = d->render(0, true);
        QCOMPARE(img.pixelColor(15, 15), QColor(Qt::green));
        QCOMPARE(img.pixelColor(115, 65), QColor(Qt::green));
        QCOMPARE(img.pixelColor(145, 65), QColor(Qt::blue));
        // growing the symbol keeps the instance's scale and position mapping
        d->find(ids[0])->rect = QRectF(110, 60, 100, 40); // 2x
        frames = d->symbol(sid)->frames;
        frames[0][1].rect = QRectF(40, 10, 40, 20);       // symbol bounds now 10..80
        d->setSymbolFrames(sid, frames);
        QCOMPARE(d->find(ids[0])->rect, QRectF(110, 60, 140, 40));
        QCOMPARE(d->objects(0)[0].rect, QRectF(10, 10, 70, 20));
        d->commit(QStringLiteral("Edit Symbol"));
        img = d->render(0, true);
        QCOMPARE(img.pixelColor(195, 75), QColor(Qt::blue));
        // undo restores the symbol and its instances
        d->undo();
        QCOMPARE(d->symbol(sid)->frames[0][0].fill, QColor(Qt::green));
        QCOMPARE(d->symbol(sid)->frames[0][1].rect, QRectF(40, 10, 20, 20));
        d->undo();
        QCOMPARE(d->symbol(sid)->frames[0][0].fill, QColor(Qt::red));
        d->redo();
        // break apart -> plain objects at the instance's place and scale
        d->setSelection({ids[0]});
        auto parts = d->breakApart();
        QCOMPARE(parts.size(), 2);
        QCOMPARE(d->find(parts[0])->rect, QRectF(110, 60, 20, 20));
        QCOMPARE(d->find(parts[1])->rect, QRectF(140, 60, 20, 20));
        QCOMPARE(d->instanceCount(sid), 1);
        // delete the symbol removes the remaining instance
        QVERIFY(d->deleteSymbol(sid));
        QCOMPARE(d->instanceCount(sid), 0);
        QVERIFY(d->s.symbols.isEmpty());
        QCOMPARE(d->objects(0).size(), 2);
    }
    void symbolsKindsAndDuplicate() {
        std::unique_ptr<Document> d(Document::createNew(100, 100, Qt::white));
        VObject a = mkRect(d.get(), QRectF(0, 0, 10, 10), Qt::red);
        d->addObject(a, 0, true);
        d->setSelection({a.id});
        const quint64 bid = d->convertToSymbol(QStringLiteral("Btn"), SymbolKind::Button);
        QCOMPARE(d->symbol(bid)->frames.size(), 4);              // Up, Over, Down, Over While Down
        QCOMPARE(d->symbol(bid)->frames[2][0].fill, QColor(Qt::red)); // states start as copies of Up
        QCOMPARE(d->uniqueSymbolName(QStringLiteral("Btn")), QStringLiteral("Btn 2"));
        const quint64 dup = d->duplicateSymbol(bid);
        QVERIFY(dup && dup != bid);
        QCOMPARE(d->symbol(dup)->name, QStringLiteral("Btn 2"));
        QVERIFY(d->symbol(dup)->frames[0][0].id != d->symbol(bid)->frames[0][0].id);
        QCOMPARE(d->instanceCount(dup), 0);
        // button instance shows its Over state when symFrame = 1
        auto fr = d->symbol(bid)->frames;
        fr[1][0].fill = QColor(Qt::blue);
        d->setSymbolFrames(bid, fr);
        QCOMPARE(d->render(0, true).pixelColor(5, 5), QColor(Qt::red));
        d->objects(0)[0].symFrame = 1;
        QCOMPARE(d->render(0, true).pixelColor(5, 5), QColor(Qt::blue));
        // graphic symbols keep one frame
        Symbol g;
        g.name = QStringLiteral("G");
        g.frames = {{a}, {a}};
        const quint64 gid = d->addSymbol(g);
        QCOMPARE(d->symbol(gid)->frames.size(), 1);
        // nested instances render (and a self-reference doesn't recurse forever)
        VObject nested = d->makeInstance(bid);
        nested.id = d->newId();
        d->setSymbolFrames(gid, {{nested}});
        VObject gi = d->makeInstance(gid);
        gi.id = d->newId();
        gi.rect = QRectF(50, 50, 20, 20);
        d->addObject(gi, 0, false);
        QCOMPARE(d->render(0, true).pixelColor(60, 60), QColor(Qt::red));
        VObject self = d->makeInstance(gid);
        self.id = d->newId();
        auto gf = d->symbol(gid)->frames;
        gf[0] << self;
        d->symbol(gid)->frames = gf;
        (void)d->render(0, true); // must terminate
    }
    void symbolsTweenAndAnimate() {
        std::unique_ptr<Document> d(Document::createNew(300, 100, Qt::white));
        VObject a = mkRect(d.get(), QRectF(0, 0, 10, 10), Qt::red);
        d->addObject(a, 0, true);
        d->setSelection({a.id});
        const quint64 sid = d->convertToSymbol(QStringLiteral("Dot"), SymbolKind::Graphic);
        const quint64 i1 = *d->selection.begin();
        auto i2 = d->insertInstance(sid, QPointF(105, 5)); // rect 100..110
        d->find(i2[0])->opacity = 0;
        d->setSelection({i1, i2[0]});
        QVERIFY(d->tweenInstances(4, false));
        QCOMPARE(d->objects(0).size(), 6);
        QCOMPARE(d->instanceCount(sid), 6);
        QCOMPARE(d->objects(0)[2].rect.left(), 40.0);
        QCOMPARE(d->objects(0)[2].opacity, 60);
        QCOMPARE(d->selection.size(), 6);
        // different symbols can't be tweened
        VObject b = mkRect(d.get(), QRectF(0, 50, 10, 10), Qt::blue);
        d->addObject(b, 0, true);
        d->setSelection({b.id});
        d->convertToSymbol(QStringLiteral("Other"), SymbolKind::Graphic);
        d->setSelection({i1, *d->selection.begin()});
        QVERIFY(!d->tweenInstances(3, false));
        // distribute to frames
        std::unique_ptr<Document> e(Document::createNew(300, 100, Qt::white));
        VObject c = mkRect(e.get(), QRectF(0, 0, 10, 10), Qt::red);
        e->addObject(c, 0, true);
        e->setSelection({c.id});
        const quint64 s2 = e->convertToSymbol(QStringLiteral("Dot"), SymbolKind::Graphic);
        const quint64 j1 = *e->selection.begin();
        auto j2 = e->insertInstance(s2, QPointF(205, 5));
        e->setSelection({j1, j2[0]});
        QVERIFY(e->tweenInstances(3, true));
        QCOMPARE(e->s.frames.size(), 5);
        for (int f = 0; f < 5; ++f) QCOMPARE(e->s.frames[f].layers[0].size(), 1);
        QCOMPARE(e->s.frames[4].layers[0][0].rect.left(), 200.0);
        QCOMPARE(e->render(2, true).pixelColor(105, 5), QColor(Qt::red));
        // Animate Selection: 5 frames, moving 100 px and fading out
        std::unique_ptr<Document> m(Document::createNew(300, 100, Qt::white));
        VObject r = mkRect(m.get(), QRectF(0, 0, 20, 20), Qt::red);
        m->addObject(r, 0, true);
        m->setSelection({r.id});
        const quint64 an = m->animateSelection(5, QPointF(100, 0), 100, 100, 0);
        QVERIFY(an);
        QCOMPARE(m->symbol(an)->kind, SymbolKind::Animation);
        QCOMPARE(m->symbol(an)->frames.size(), 5);
        QCOMPARE(m->s.frames.size(), 5);
        QCOMPARE(m->render(0, true).pixelColor(5, 5), QColor(Qt::red));
        QCOMPARE(m->render(2, true).pixelColor(55, 5).red(), 255);
        QVERIFY(m->render(2, true).pixelColor(55, 5).green() > 100); // half faded
        QCOMPARE(m->render(2, true).pixelColor(5, 5), QColor(Qt::white));
        QCOMPARE(m->render(4, true).pixelColor(105, 5), QColor(Qt::white)); // opacity 0
        // inserting an animation symbol spans frames
        m->setCurrentFrame(0);
        auto span = m->insertInstance(an, QPointF(60, 60));
        QCOMPARE(span.size(), 5);
        QCOMPARE(m->instanceCount(an), 10);
    }
    void symbolsLwdRoundtrip() {
        std::unique_ptr<Document> d(Document::createNew(120, 80, Qt::white));
        VObject a = mkRect(d.get(), QRectF(10, 10, 30, 30), Qt::red);
        VObject bm;
        bm.type = ObjType::Bitmap;
        bm.id = d->newId();
        bm.image = QImage(8, 8, QImage::Format_ARGB32);
        bm.image.fill(QColor(0, 0, 255));
        bm.rect = QRectF(50, 10, 8, 8);
        d->addObject(a, 0, false);
        d->addObject(bm, 0, false);
        d->setSelection({a.id, bm.id});
        const quint64 sid = d->convertToSymbol(QStringLiteral("Mixed"), SymbolKind::Button);
        VObject &inst = d->objects(0)[0];
        inst.link = QStringLiteral("https://example.org/");
        inst.alt = QStringLiteral("Go");
        inst.symFrame = 2;
        inst.effects << LiveEffect::make(EffectType::DropShadow);
        d->commit(QStringLiteral("x"));
        const QImage before = d->render(0, true);
        QString err;
        std::unique_ptr<Document> back(lwd::deserialize(lwd::serialize(*d), &err));
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->s.symbols.size(), 1);
        const Symbol *s = back->symbol(sid);
        QVERIFY(s);
        QCOMPARE(s->name, QStringLiteral("Mixed"));
        QCOMPARE(s->kind, SymbolKind::Button);
        QCOMPARE(s->frames.size(), 4);
        QCOMPARE(s->frames[3].size(), 2);
        QCOMPARE(s->frames[3][1].type, ObjType::Bitmap);
        QCOMPARE(s->frames[3][1].image.pixelColor(3, 3), QColor(0, 0, 255));
        const VObject &bi = back->objects(0)[0];
        QCOMPARE(bi.type, ObjType::Instance);
        QCOMPARE(bi.symbolId, sid);
        QCOMPARE(bi.symFrame, 2);
        QCOMPARE(bi.link, QStringLiteral("https://example.org/"));
        QCOMPARE(bi.alt, QStringLiteral("Go"));
        QCOMPARE(bi.effects.size(), 1);
        QCOMPARE(back->render(0, true), before);
        // new ids never collide with symbol contents
        const quint64 nid = back->newId();
        for (const auto &f : s->frames) for (const VObject &o : f) QVERIFY(o.id < nid);
        QVERIFY(sid < nid);
    }
    void symbolsHtmlButtons() {
        std::unique_ptr<Document> d(Document::createNew(100, 60, Qt::white));
        VObject a = mkRect(d.get(), QRectF(10, 10, 40, 20), Qt::red);
        d->addObject(a, 0, true);
        d->setSelection({a.id});
        const quint64 sid = d->convertToSymbol(QStringLiteral("Nav"), SymbolKind::Button);
        auto fr = d->symbol(sid)->frames;
        fr[1][0].fill = QColor(Qt::blue);
        fr[2][0].fill = QColor(Qt::green);
        d->setSymbolFrames(sid, fr);
        d->objects(0)[0].link = QStringLiteral("next.html");
        QTemporaryDir td;
        const QString path = td.filePath(QStringLiteral("page.html"));
        QString err;
        QStringList written;
        QVERIFY2(html::exportHtml(*d, path, {}, &err, &written), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString h = QString::fromUtf8(f.readAll());
        QVERIFY2(h.contains(QStringLiteral("href=\"next.html\"")), qPrintable(h));
        QVERIFY(h.contains(QStringLiteral("onmouseover=\"lwSwap('lw_Nav','images/Nav_over.png')\"")));
        QVERIFY(h.contains(QStringLiteral("onmousedown=\"lwSwap('lw_Nav','images/Nav_down.png')\"")));
        QImage over(td.filePath(QStringLiteral("images/Nav_over.png")));
        QImage down(td.filePath(QStringLiteral("images/Nav_down.png")));
        QImage up(td.filePath(QStringLiteral("images/Nav.png")));
        QVERIFY(over.width() >= 40 && over.height() >= 20);
        QCOMPARE(over.size(), up.size());
        const QPoint c(over.width() / 2, over.height() / 2);
        QCOMPARE(over.pixelColor(c), QColor(Qt::blue));
        QCOMPARE(down.pixelColor(c), QColor(Qt::green));
        QCOMPARE(up.pixelColor(c), QColor(Qt::red));
        // the source document is not modified by the export
        QCOMPARE(d->webLayer(), -1);
    }

    // ---- Milestone 5: paths
    static int filledPixels(const QPainterPath &p, int w = 200, int h = 200) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter g(&img);
        g.setRenderHint(QPainter::Antialiasing, false);
        g.fillPath(p, Qt::black);
        g.end();
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) n += qAlpha(img.pixel(x, y)) > 0;
        return n;
    }
    static Document *twoRects(quint64 *a = nullptr, quint64 *b = nullptr) {
        Document *d = Document::createNew(200, 200, Qt::white);
        VObject r1 = mkRect(d, QRectF(10, 10, 40, 40), Qt::red);   // bottom
        VObject r2 = mkRect(d, QRectF(30, 30, 40, 40), Qt::blue);  // top
        d->addObject(r1, 0, false);
        d->addObject(r2, 0, false);
        d->setSelection({r1.id, r2.id});
        d->commit(QStringLiteral("rects"));
        if (a) *a = r1.id;
        if (b) *b = r2.id;
        return d;
    }
    void pathHandlesRoundtrip() {
        std::unique_ptr<Document> d(Document::createNew(200, 200, Qt::white));
        VObject o;
        o.type = ObjType::Path;
        o.points = QPolygonF({{10, 100}, {100, 100}, {150, 50}, {20, 20}, {60, 20}, {40, 60}});
        o.hIn = o.points; o.hOut = o.points;
        o.hOut[0] = QPointF(40, 40); o.hIn[1] = QPointF(70, 40);
        o.contours = {3};
        o.closed = true;
        o.id = d->newId();
        d->addObject(o, 0, false);
        d->commit(QStringLiteral("p"));
        QVERIFY(d->objects(0)[0].hasCurves());
        QCOMPARE(d->objects(0)[0].contourCount(), 2);
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(e);
        const VObject &r = e->objects(0)[0];
        QCOMPARE(r.points, o.points);
        QCOMPARE(r.hIn, o.hIn);
        QCOMPARE(r.hOut, o.hOut);
        QCOMPARE(r.contours, o.contours);
        QVERIFY(r.closed);
        // the curve bulges up toward the handles: the bounds reach above the straight segment
        QVERIFY(r.bounds().top() < 60);
        // the moved/scaled path keeps its handles in step
        VObject m = r;
        m.moveBy(QPointF(5, 5));
        QCOMPARE(m.hOut[0], QPointF(45, 45));
    }
    void pathCombineOps() {
        quint64 a, b;
        { // union: one object, area 1600+1600-400, bottom object's colour
            std::unique_ptr<Document> d(twoRects(&a, &b));
            QVERIFY(paths::combine(*d, paths::Combine::Union));
            d->commit(QStringLiteral("Union"));
            QCOMPARE(d->objects(0).size(), 1);
            const VObject &u = d->objects(0)[0];
            QCOMPARE(u.type, ObjType::Path);
            QCOMPARE(u.fill, QColor(Qt::red));
            QVERIFY(qAbs(filledPixels(u.shapePath()) - 2800) < 120);
            d->undo();
            QCOMPARE(d->objects(0).size(), 2);
        }
        { // intersect: 20x20
            std::unique_ptr<Document> d(twoRects());
            QVERIFY(paths::combine(*d, paths::Combine::Intersect));
            QCOMPARE(d->objects(0).size(), 1);
            QVERIFY(qAbs(filledPixels(d->objects(0)[0].shapePath()) - 400) < 60);
        }
        { // punch: the top object cuts the bottom one and disappears
            std::unique_ptr<Document> d(twoRects(&a, &b));
            QVERIFY(paths::combine(*d, paths::Combine::Punch));
            QCOMPARE(d->objects(0).size(), 1);
            QCOMPARE(d->objects(0)[0].fill, QColor(Qt::red));
            QVERIFY(qAbs(filledPixels(d->objects(0)[0].shapePath()) - 1200) < 80);
        }
        { // crop: what is left of the bottom object inside the top one
            std::unique_ptr<Document> d(twoRects());
            QVERIFY(paths::combine(*d, paths::Combine::Crop));
            QCOMPARE(d->objects(0).size(), 1);
            QCOMPARE(d->objects(0)[0].fill, QColor(Qt::red));
            QVERIFY(qAbs(filledPixels(d->objects(0)[0].shapePath()) - 400) < 60);
        }
        { // needs two objects
            std::unique_ptr<Document> d(twoRects(&a, &b));
            d->setSelection({a});
            QString err;
            QVERIFY(!paths::combine(*d, paths::Combine::Union, &err));
            QVERIFY(!err.isEmpty());
        }
        { // join -> compound path with a hole; split -> two paths again
            std::unique_ptr<Document> d(Document::createNew(200, 200, Qt::white));
            VObject outer = mkRect(d.get(), QRectF(10, 10, 100, 100), Qt::red);
            VObject inner = mkRect(d.get(), QRectF(40, 40, 20, 20), Qt::blue);
            d->addObject(outer, 0, false);
            d->addObject(inner, 0, false);
            d->setSelection({outer.id, inner.id});
            QVERIFY(paths::combine(*d, paths::Combine::Join));
            d->commit(QStringLiteral("Join"));
            QCOMPARE(d->objects(0).size(), 1);
            const VObject &j = d->objects(0)[0];
            QCOMPARE(j.contourCount(), 2);
            QVERIFY(qAbs(filledPixels(j.shapePath()) - 9600) < 200); // even-odd hole
            d->setSelection({j.id});
            QCOMPARE(paths::split(*d), 1);
            d->commit(QStringLiteral("Split"));
            QCOMPARE(d->objects(0).size(), 2);
            for (const VObject &o : d->objects(0)) QCOMPARE(o.contourCount(), 1);
        }
    }
    void pathConvertAndAlter() {
        std::unique_ptr<Document> d(Document::createNew(200, 200, Qt::white));
        VObject e;
        e.type = ObjType::Ellipse;
        e.rect = QRectF(20, 20, 100, 60);
        e.fill = Qt::green;
        e.id = d->newId();
        d->addObject(e, 0, false);
        d->setSelection({e.id});
        const int before = filledPixels(d->objects(0)[0].shapePath());
        QCOMPARE(paths::convertToPaths(*d), 1);
        d->commit(QStringLiteral("Convert to Paths"));
        const VObject &p = d->objects(0)[0];
        QCOMPARE(p.type, ObjType::Path);
        QVERIFY(p.hasCurves());
        QVERIFY(p.closed);
        QVERIFY(p.points.size() >= 4 && p.points.size() <= 8);
        QVERIFY(qAbs(filledPixels(p.shapePath()) - before) < before / 50);
        QVERIFY(qAbs(p.bounds().width() - 100) < 1.5);

        // simplify: a densely sampled circle collapses to far fewer straight nodes
        paths::densify(d->objects(0)[0], 1.0);
        const int dense = d->objects(0)[0].points.size();
        QVERIFY(dense > 100);
        QCOMPARE(paths::simplify(*d, 2.0), 1);
        QVERIFY(d->objects(0)[0].points.size() < dense / 3);
        QVERIFY(qAbs(filledPixels(d->objects(0)[0].shapePath()) - before) < before / 20);

        // inset inside shrinks, outside grows
        const int s0 = filledPixels(d->objects(0)[0].shapePath());
        QCOMPARE(paths::insetPath(*d, 5, true), 1);
        const int s1 = filledPixels(d->objects(0)[0].shapePath());
        QVERIFY(s1 < s0 * 0.8);
        QCOMPARE(paths::insetPath(*d, 5, false), 1);
        QVERIFY(qAbs(filledPixels(d->objects(0)[0].shapePath()) - s0) < s0 / 15);

        // expand stroke: an open line becomes a filled outline with the stroke colour
        VObject l;
        l.type = ObjType::Path;
        l.points = QPolygonF({{10, 150}, {190, 150}});
        l.strokeOn = true;
        l.stroke = Qt::magenta;
        l.strokeWidth = 4;
        l.fillOn = false;
        l.id = d->newId();
        d->addObject(l, 0, false);
        d->setSelection({l.id});
        QCOMPARE(paths::expandStroke(*d, 10, Qt::MiterJoin, Qt::FlatCap), 1);
        const VObject *x = d->find(l.id);
        QVERIFY(x);
        QVERIFY(x->fillOn);
        QVERIFY(!x->strokeOn);
        QCOMPARE(x->fill, QColor(Qt::magenta));
        QVERIFY(x->closed);
        QVERIFY(qAbs(filledPixels(x->shapePath()) - 1800) < 120);
    }
    void pathKnifeAndSculpt() {
        std::unique_ptr<Document> d(Document::createNew(200, 200, Qt::white));
        VObject r = mkRect(d.get(), QRectF(20, 20, 100, 100), Qt::red);
        r.strokeOn = true;
        d->addObject(r, 0, false);
        d->setSelection({r.id});
        d->commit(QStringLiteral("rect"));
        // a vertical cut through the middle splits the closed outline into two open pieces
        QCOMPARE(paths::knife(*d, QPolygonF({{70, 0}, {70, 200}})), 1);
        d->commit(QStringLiteral("Knife"));
        QCOMPARE(d->objects(0).size(), 2);
        for (const VObject &o : d->objects(0)) {
            QCOMPARE(o.type, ObjType::Path);
            QVERIFY(!o.closed);
            QVERIFY(o.bounds().width() <= 51);
        }
        QCOMPARE(d->selection.size(), 2);
        d->undo();
        QCOMPARE(d->objects(0).size(), 1);
        // a cut that misses changes nothing
        d->setSelection({r.id});
        QCOMPARE(paths::knife(*d, QPolygonF({{150, 0}, {150, 200}})), 0);

        // knife through a curve: the pieces keep their curve handles
        VObject e;
        e.type = ObjType::Ellipse;
        e.rect = QRectF(20, 20, 100, 100);
        e.id = d->newId();
        d->addObject(e, 0, false);
        d->setSelection({e.id});
        QCOMPARE(paths::knife(*d, QPolygonF({{0, 70}, {200, 70}})), 1);
        int curved = 0;
        for (const VObject &o : d->objects(0)) if (o.type == ObjType::Path && o.hasCurves()) ++curved;
        QCOMPARE(curved, 2);

        // freeform push / pull
        VObject sq = mkRect(d.get(), QRectF(50, 50, 100, 100), Qt::red);
        QVERIFY(paths::toPath(sq));
        paths::densify(sq, 4);
        QVERIFY(sq.points.size() > 50);
        const QRectF b0 = sq.bounds();
        QVERIFY(paths::pull(sq, QPointF(100, 50), QPointF(0, -20), 20, 1.0)); // pull the top edge up
        QVERIFY(sq.bounds().top() < b0.top() - 15);
        QVERIFY(qAbs(sq.bounds().bottom() - b0.bottom()) < 0.01);
        QVERIFY(paths::push(sq, QPointF(150, 100), 15)); // push the right edge out of the circle
        bool inside = false;
        for (const QPointF &pt : sq.points) if (QLineF(pt, QPointF(150, 100)).length() < 14.9) inside = true;
        QVERIFY(!inside);
        const int n0 = sq.points.size();
        sq.points[1] += QPointF(0, -40);
        paths::refine(sq, 4);
        QVERIFY(sq.points.size() > n0);
    }

    void gradientFills() {
        std::unique_ptr<Document> d(Document::createNew(200, 100, Qt::white));
        VObject r = mkRect(d.get(), QRectF(0, 0, 200, 100), Qt::red);
        fills::setFillKind(r, FillKind::Linear);
        r.gradient = {{0, Qt::red}, {1, Qt::blue}};
        QCOMPARE(r.gradP0, QPointF(0, 0.5));
        d->addObject(r, 0, false);
        QImage img = d->render(0, true);
        QVERIFY(qRed(img.pixel(3, 50)) > 220 && qBlue(img.pixel(3, 50)) < 40);
        QVERIFY(qBlue(img.pixel(196, 50)) > 220 && qRed(img.pixel(196, 50)) < 40);
        // the handles follow the object's bounds
        VObject &o = d->objects(0)[0];
        fills::setGradientVector(o, QPointF(100, 0), QPointF(100, 100)); // vertical
        QVERIFY(qAbs(fills::handleToDoc(o, o.gradP1).y() - 100) < 1e-6);
        img = d->render(0, true);
        QVERIFY(qRed(img.pixel(100, 3)) > 220);
        QVERIFY(qBlue(img.pixel(100, 96)) > 220);
        // every gradient type renders something different from a solid fill
        for (int k = int(FillKind::Linear); k <= int(FillKind::Rectangle); ++k) {
            fills::setFillKind(o, FillKind(k));
            QImage g = d->render(0, true);
            QSet<QRgb> colors;
            for (int x = 0; x < 200; x += 10) for (int y = 0; y < 100; y += 10) colors.insert(g.pixel(x, y));
            QVERIFY2(colors.size() > 3, qPrintable(fills::fillKindKey(FillKind(k))));
        }
        QCOMPARE(fills::sampleGradient({{0, Qt::black}, {1, Qt::white}}, 0.5).red(), 128);
        QVERIFY(fills::gradientPresetNames().contains(QStringLiteral("Spectrum")));
    }

    void patternTextureEdge() {
        std::unique_ptr<Document> d(Document::createNew(100, 100, Qt::white));
        VObject r = mkRect(d.get(), QRectF(10, 10, 80, 80), Qt::black);
        r.fillKind = FillKind::Pattern;
        r.pattern = QStringLiteral("Checkerboard");
        d->addObject(r, 0, false);
        QImage img = d->render(0, true);
        QSet<QRgb> c;
        for (int x = 12; x < 88; ++x) c.insert(img.pixel(x, 50));
        QVERIFY(c.size() >= 2);
        for (const QString &n : fills::patternNames()) QVERIFY(!fills::patternTile(n).isNull());
        // texture darkens part of a solid fill
        VObject &o = d->objects(0)[0];
        o.fillKind = FillKind::Solid;
        o.fill = Qt::white;
        o.texture = QStringLiteral("Noise");
        o.textureAmount = 80;
        img = d->render(0, true);
        int dark = 0;
        for (int x = 12; x < 88; ++x) if (qRed(img.pixel(x, 50)) < 200) ++dark;
        QVERIFY(dark > 5);
        // feather: soft edge
        o.texture.clear();
        o.fill = Qt::black;
        o.feather = 8;
        img = d->render(0, true);
        const int edge = qRed(img.pixel(10, 50));
        QVERIFY(edge > 40 && edge < 220);
        QVERIFY(qRed(img.pixel(50, 50)) < 10);
        // hard edge: no partial pixels
        o.feather = 0;
        o.antialias = false;
        o.rect = QRectF(10.5, 10.5, 80, 80);
        img = d->render(0, true);
        for (int x = 0; x < 100; ++x) { const int v = qRed(img.pixel(x, 50)); QVERIFY(v == 0 || v == 255); }
    }

    void strokeCategoriesAndAlign() {
        std::unique_ptr<Document> d(Document::createNew(100, 100, Qt::white));
        VObject r = mkRect(d.get(), QRectF(20, 20, 60, 60), Qt::white);
        r.fillOn = false;
        r.strokeOn = true;
        r.stroke = Qt::black;
        r.strokeWidth = 10;
        r.strokeAlign = 1; // inside
        d->addObject(r, 0, false);
        QImage img = d->render(0, true);
        QVERIFY(qRed(img.pixel(18, 50)) > 250);  // nothing outside
        QVERIFY(qRed(img.pixel(24, 50)) < 10);
        d->objects(0)[0].strokeAlign = 2; // outside
        img = d->render(0, true);
        QVERIFY(qRed(img.pixel(16, 50)) < 10);
        QVERIFY(qRed(img.pixel(23, 50)) > 250);
        QVERIFY(fills::strokePresets().size() >= 30);
        QVERIFY(fills::strokeCategories().contains(QStringLiteral("Charcoal")));
        // every stroke category draws something
        d->objects(0)[0].strokeAlign = 0;
        d->objects(0)[0].strokeWidth = 6;
        for (const auto &sp : fills::strokePresets()) {
            d->objects(0)[0].strokeKind = sp.key;
            img = d->render(0, true);
            int ink = 0;
            for (int y = 10; y < 90; ++y) for (int x = 10; x < 30; ++x) if (qRed(img.pixel(x, y)) < 200) ++ink;
            QVERIFY2(ink > 3, qPrintable(sp.key));
        }
    }

    void fillsLwdRoundTrip() {
        std::unique_ptr<Document> d(Document::createNew(100, 100, Qt::white));
        VObject r = mkRect(d.get(), QRectF(10, 10, 80, 80), Qt::red);
        fills::setFillKind(r, FillKind::Satin);
        r.gradient = fills::gradientPreset(QStringLiteral("Copper"));
        r.gradP1 = QPointF(0.8, 0.2);
        r.texture = QStringLiteral("Canvas");
        r.textureAmount = 33;
        r.feather = 3;
        r.strokeKind = QStringLiteral("charcoal.soft");
        r.strokeAlign = 2;
        d->addObject(r, 0, false);
        VObject p = mkRect(d.get(), QRectF(0, 0, 20, 20), Qt::red);
        p.fillKind = FillKind::Pattern;
        p.pattern = QStringLiteral("Custom");
        p.patternImage = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
        p.patternImage.fill(Qt::green);
        d->addObject(p, 0, false);
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(e);
        const VObject &a = e->objects(0)[0];
        QCOMPARE(a.fillKind, FillKind::Satin);
        QCOMPARE(a.gradient, r.gradient);
        QVERIFY(QLineF(a.gradP1, r.gradP1).length() < 1e-6);
        QCOMPARE(a.texture, r.texture);
        QCOMPARE(a.textureAmount, 33);
        QCOMPARE(a.feather, 3.0);
        QCOMPARE(a.strokeKind, r.strokeKind);
        QCOMPARE(a.strokeAlign, 2);
        const VObject &b = e->objects(0)[1];
        QCOMPARE(b.pattern, QStringLiteral("Custom"));
        QCOMPARE(b.patternImage.size(), QSize(8, 8));
        QCOMPARE(QColor(b.patternImage.pixel(3, 3)), QColor(Qt::green));
    }

    void textOnPath() {
        std::unique_ptr<Document> d(Document::createNew(300, 200, Qt::white));
        VObject t;
        t.type = ObjType::Text;
        t.text = QStringLiteral("Hello curve");
        t.fontSize = 20;
        t.fillOn = true;
        t.fill = Qt::black;
        t.rect = QRectF(10, 10, 10, 10);
        t.fitTextBox();
        t.id = d->newId();
        d->addObject(t, 0, false);
        VObject e;
        e.type = ObjType::Ellipse;
        e.rect = QRectF(50, 20, 200, 160);
        e.strokeOn = true;
        e.id = d->newId();
        d->addObject(e, 0, false);
        d->commit(QStringLiteral("setup"));
        d->setSelection({t.id, e.id});
        QVERIFY(paths::attachTextToPath(*d));
        d->commit(QStringLiteral("Attach to Path"));
        QCOMPARE(d->objects(0).size(), 1);
        const VObject &a = d->objects(0)[0];
        QVERIFY(a.textOnPath);
        QVERIFY(a.bounds().intersects(QRectF(50, 20, 200, 160)));
        QVERIFY(a.bounds().top() > 0);
        QVERIFY(!a.textPath().isEmpty());
        // saved and loaded
        std::unique_ptr<Document> l(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(l->objects(0)[0].textOnPath);
        QCOMPARE(l->objects(0)[0].points.size(), a.points.size());
        // detach recreates the path (it appends objects, so `a` must not be used past this point)
        const quint64 aId = a.id;
        d->setSelection({aId});
        QCOMPARE(paths::detachTextFromPath(*d), 1);
        QCOMPARE(d->objects(0).size(), 2);
        int nPath = 0;
        for (const VObject &o : d->objects(0)) if (o.type == ObjType::Path) ++nPath;
        QCOMPARE(nPath, 1);
        d->commit(QStringLiteral("Detach from Path"));
        d->undo();
        QCOMPARE(d->objects(0).size(), 1);
        QVERIFY(d->objects(0)[0].textOnPath);
        // selection must be text + path
        d->setSelection({aId});
        QString err;
        QVERIFY(!paths::attachTextToPath(*d, &err));
        QVERIFY(!err.isEmpty());
    }

    void onionSkinFrames() {
        std::unique_ptr<Document> d(Document::createNew(50, 50, Qt::white));
        d->addFrame(false); d->addFrame(false); d->addFrame(false);
        d->setCurrentFrame(1);
        QVERIFY(d->onionFrames().isEmpty());
        d->setOnion(1);
        QCOMPARE(d->onionFrames(), QVector<int>({2}));
        d->setOnion(2);
        QCOMPARE(d->onionFrames(), QVector<int>({0, 2}));
        d->setOnion(3);
        QCOMPARE(d->onionFrames(), QVector<int>({0, 2, 3}));
    }
    // v0.3.1: the document resolution (set by File > New, e.g. from a clipboard image) is kept in .lwd
    // and written to PNG (pHYs) and JPEG (JFIF density) exports; 72 ppi stays unwritten as before
    void documentResolution() {
        std::unique_ptr<Document> d(Document::createNew(120, 80, Qt::white, nullptr, 300));
        QCOMPARE(d->s.dpi, 300.0);
        std::unique_ptr<Document> back(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(back);
        QCOMPARE(back->s.dpi, 300.0);
        std::unique_ptr<Document> plain(Document::createNew(120, 80, Qt::white));
        QCOMPARE(plain->s.dpi, 72.0);
        QVERIFY(!lwd::serialize(*plain).contains("\"resolution\""));
        std::unique_ptr<Document> back72(lwd::deserialize(lwd::serialize(*plain)));
        QCOMPARE(back72->s.dpi, 72.0);
        for (Fmt f : {Fmt::PNG, Fmt::JPEG}) {
            ExportOptions o;
            o.format = f;
            QByteArray out;
            QString err;
            QVERIFY2(docio::exportDocument(*d, 0, o, out, &err), qPrintable(err));
            const QImage img = QImage::fromData(out);
            QCOMPARE(img.size(), QSize(120, 80));
            QCOMPARE(qRound(img.dotsPerMeterX() * 0.0254), 300);
            QByteArray out72;
            QVERIFY(docio::exportDocument(*plain, 0, o, out72, &err));
            if (f == Fmt::PNG) QVERIFY(!out72.contains("pHYs"));   // unchanged output for 72 ppi documents
            else QCOMPARE(QImage::fromData(out72).size(), QSize(120, 80));
        }
    }
    void corePrefsUndoAndInterpolation() {
        const int oldUndo = coreprefs::undoSteps, oldInterp = coreprefs::interpolation;
        coreprefs::undoSteps = 3;
        std::unique_ptr<Document> d(Document::createNew(20, 10, Qt::white));
        for (int i = 0; i < 8; ++i) { d->s.width = 20 + i; d->commit(QStringLiteral("step")); }
        QCOMPARE(d->history().size(), 4);
        int undos = 0;
        while (d->canUndo()) { d->undo(); ++undos; }
        QCOMPARE(undos, 3);
        QCOMPARE(d->s.width, 24);
        // Nearest neighbour keeps hard pixel edges when scaling bitmaps
        coreprefs::interpolation = coreprefs::Nearest;
        std::unique_ptr<Document> b(Document::createNew(2, 1, Qt::white));
        VObject o;
        o.type = ObjType::Bitmap;
        o.image = QImage(2, 1, QImage::Format_ARGB32_Premultiplied);
        o.image.setPixel(0, 0, qRgb(255, 0, 0));
        o.image.setPixel(1, 0, qRgb(0, 0, 255));
        o.rect = QRectF(0, 0, 2, 1);
        o.id = b->newId();
        b->addObject(o, 0, false);
        b->scaleImage(8, 4);
        const QImage &im = b->s.frames[0].layers[0][0].image;
        QCOMPARE(im.size(), QSize(8, 4));
        QCOMPARE(im.pixel(3, 2), qRgb(255, 0, 0));
        QCOMPARE(im.pixel(4, 2), qRgb(0, 0, 255));
        coreprefs::undoSteps = oldUndo;
        coreprefs::interpolation = oldInterp;
    }
};

QTEST_MAIN(Features)
#include "features_test.moc"
