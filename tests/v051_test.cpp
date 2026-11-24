#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QTabWidget>
#include <QPainter>
#include "core/Stroke.h"
#include "core/Fills.h"
#include "core/BrushEngine.h"
#include "core/Document.h"
#include "io/LwdFormat.h"
#include "ui/EditStrokeDialog.h"

class V051Test : public QObject {
    Q_OBJECT
    static int columnThickness(const QImage &img, int x, int thresh = 32)
    {
        int y0 = -1, y1 = -1;
        for (int y = 0; y < img.height(); ++y) {
            if (qAlpha(img.pixel(x, y)) > thresh) { if (y0 < 0) y0 = y; y1 = y; }
        }
        return y0 < 0 ? 0 : (y1 - y0 + 1);
    }
    static int rowThickness(const QImage &img, int y, int thresh = 32)
    {
        int x0 = -1, x1 = -1;
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) > thresh) { if (x0 < 0) x0 = x; x1 = x; }
        }
        return x0 < 0 ? 0 : (x1 - x0 + 1);
    }
    static QImage renderKey(const QString &key, const QPainterPath &path, double width, int W, int H, bool bitmap,
                            const QColor &color = QColor(20, 20, 40), const stroke::Params *custom = nullptr)
    {
        QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        const stroke::Params sp = custom ? *custom : stroke::paramsFor(key);
        if (bitmap) {
            brush::Params bp;
            bp.category = brush::categoryFromName(sp.category);
            brush::applyTip(bp, bp.category, brush::tipIndex(bp.category, sp.name));
            bp.strokeKey = custom ? custom->key : bp.strokeKey;
            bp.size = width;
            bp.color = color;
            brush::Stroke s;
            QImage tgt = img;
            s.begin(tgt, bp);
            QPointF last;
            bool first = true;
            auto walk = [&](const QPainterPath &p) {
                for (const QPolygonF &poly : p.toSubpathPolygons()) {
                    for (const QPointF &pt : poly) {
                        if (first) { s.composite(tgt, s.segment(pt, pt)); first = false; }
                        else s.composite(tgt, s.segment(last, pt));
                        last = pt;
                    }
                }
            };
            walk(path);
            s.composite(tgt, s.finish());
            s.end();
            return tgt;
        }
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        stroke::PaintInput in;
        in.params = sp;
        in.width = width;
        in.color = color;
        in.seed = 0x51u;
        stroke::paint(p, path, in);
        return img;
    }
private slots:
    void uniqueNamesAndParams() {
        QSet<QString> keys;
        QMap<QString, QSet<QString>> names;
        QHash<QString, QString> fingerprints;
        QVERIFY(stroke::presets().size() >= 40);
        for (const auto &sp : stroke::presets()) {
            QVERIFY2(!sp.key.isEmpty() && !sp.category.isEmpty() && !sp.name.isEmpty(), qPrintable(sp.key));
            QVERIFY2(!keys.contains(sp.key), qPrintable(sp.key));
            keys.insert(sp.key);
            QVERIFY2(!names[sp.category].contains(sp.name), qPrintable(sp.category + QLatin1Char('/') + sp.name));
            names[sp.category].insert(sp.name);
            const QString fp = stroke::paramFingerprint(stroke::paramsFor(sp.key));
            if (fingerprints.contains(fp))
                QFAIL(qPrintable(QStringLiteral("identical params: %1 and %2").arg(fingerprints[fp], sp.key)));
            fingerprints.insert(fp, sp.key);
        }
        // Names unique per category already asserted; categories themselves unique in the menu list
        const QStringList cats = stroke::categories();
        QCOMPARE(QSet<QString>(cats.begin(), cats.end()).size(), cats.size());
    }

    void verifiedFireworksPresetsPresent() {
        auto has = [](const QString &key) { return stroke::factory(key) && stroke::factory(key)->key == key; };
        auto named = [](const QString &key, const QString &name) {
            const stroke::Params *p = stroke::factory(key);
            return p && p->name == name;
        };
        // Official CS5 help (S1)
        QVERIFY(named(QStringLiteral("calligraphy.bamboo"), QStringLiteral("Bamboo")));
        QVERIFY(named(QStringLiteral("calligraphy.ribbon"), QStringLiteral("Ribbon")));
        QVERIFY(named(QStringLiteral("felttip.dark"), QStringLiteral("Dark Marker")));
        QVERIFY(named(QStringLiteral("felttip.light"), QStringLiteral("Light Marker")));
        QVERIFY(named(QStringLiteral("oil.splatter"), QStringLiteral("Splattered Oil")));
        QVERIFY(named(QStringLiteral("random.confetti"), QStringLiteral("Confetti")));
        QVERIFY(named(QStringLiteral("unnatural.3d"), QStringLiteral("3D")));
        QVERIFY(named(QStringLiteral("unnatural.toothpaste"), QStringLiteral("Toothpaste")));
        QVERIFY(named(QStringLiteral("unnatural.alienPaint"), QStringLiteral("Viscous Alien Paint")));
        // Japanese CS5 tutorial (S6)
        QVERIFY(named(QStringLiteral("unnatural.outline"), QStringLiteral("Outline")));
        QVERIFY(named(QStringLiteral("unnatural.fluidSplatter"), QStringLiteral("Fluid Splatter")));
        // S7 watercolor random; S9 pencil
        QVERIFY(named(QStringLiteral("watercolor.random"), QStringLiteral("Random")));
        QVERIFY(named(QStringLiteral("pencil.1pxSoft"), QStringLiteral("1-Pixel Soft")));
        // S3 categories + S4 Dashed
        for (const char *c : {"Pencil", "Basic", "Air Brush", "Calligraphy", "Charcoal", "Crayon",
                              "Felt Tip", "Oil", "Watercolor", "Random", "Unnatural", "Dashed"})
            QVERIFY2(stroke::categories().contains(QString::fromLatin1(c)), c);
        QCOMPARE(stroke::namesInCategory(QStringLiteral("Dashed")).size(), 6);
        // Brief's Unnatural set (some unverified — see fireworks-brushes.md)
        for (const char *n : {"3D", "3D Glow", "Chameleon", "Fluid Splatter", "Outline", "Paint Splatter",
                              "Squiggle", "Toothpaste", "Toxic Waste", "Viscous Alien Paint"})
            QVERIFY2(stroke::namesInCategory(QStringLiteral("Unnatural")).contains(QString::fromLatin1(n)), n);
        // Old key alias
        QCOMPARE(stroke::canonicalKey(QStringLiteral("oil.splattered")), QStringLiteral("oil.splatter"));
        Q_UNUSED(has);
        // Libreworks category follows the Fireworks set
        QVERIFY(stroke::categories().contains(QStringLiteral("Libreworks")));
        QCOMPARE(stroke::categories().last(), QStringLiteral("Libreworks"));
    }

    void libreworksPresetsPresent() {
        const QStringList expected = {
            QStringLiteral("Neon Tube"), QStringLiteral("Gouache"), QStringLiteral("Ink Wash (Sumi-e)"),
            QStringLiteral("Pixel Pencil"), QStringLiteral("Ribbon Twist"), QStringLiteral("Chain"),
            QStringLiteral("Rope"), QStringLiteral("Stitch"), QStringLiteral("Glitter"),
            QStringLiteral("Smoke"), QStringLiteral("Fire"), QStringLiteral("Rainbow"),
            QStringLiteral("Lightning"), QStringLiteral("Spray Can"), QStringLiteral("Image Stamp"),
            QStringLiteral("Cellular"), QStringLiteral("Cellular HighLife"), QStringLiteral("Cellular Seeds"),
            QStringLiteral("Cellular Day & Night"), QStringLiteral("Cellular Brian's Brain"),
            QStringLiteral("Cellular Wolfram 110")
        };
        const QStringList names = stroke::namesInCategory(QStringLiteral("Libreworks"));
        QCOMPARE(names.size(), expected.size());
        for (const QString &n : expected)
            QVERIFY2(names.contains(n), qPrintable(n));
        const char *keys[] = {
            "lw.neonTube", "lw.gouache", "lw.inkWash", "lw.pixelPencil", "lw.ribbonTwist",
            "lw.chain", "lw.rope", "lw.stitch", "lw.glitter", "lw.smoke", "lw.fire",
            "lw.rainbow", "lw.lightning", "lw.sprayCan", "lw.imageStamp",
            "lw.cellular", "lw.cellularHighLife", "lw.cellularSeeds",
            "lw.cellularDayNight", "lw.cellularBrain", "lw.cellularWolfram110"
        };
        for (const char *k : keys) {
            const stroke::Params *p = stroke::factory(QString::fromLatin1(k));
            QVERIFY2(p && p->key == QLatin1String(k) && p->category == QLatin1String("Libreworks"), k);
        }
        QCOMPARE(brush::categoryName(brush::Category::Libreworks), QStringLiteral("Libreworks"));
        QVERIFY(brush::tipNames(brush::Category::Libreworks).contains(QStringLiteral("Neon Tube")));
    }

    void strokesAreDeterministic() {
        QPainterPath path;
        path.moveTo(16, 28);
        path.cubicTo(70, 6, 110, 50, 200, 28);
        const QStringList keys = {
            QStringLiteral("lw.glitter"), QStringLiteral("lw.lightning"), QStringLiteral("lw.sprayCan"),
            QStringLiteral("lw.fire"), QStringLiteral("lw.smoke"), QStringLiteral("lw.imageStamp"),
            QStringLiteral("lw.inkWash"), QStringLiteral("random.confetti"),
            QStringLiteral("lw.cellular"), QStringLiteral("lw.cellularBrain"), QStringLiteral("lw.cellularWolfram110")
        };
        for (const QString &key : keys) {
            const QImage a = renderKey(key, path, 10, 220, 48, false);
            const QImage b = renderKey(key, path, 10, 220, 48, false);
            QVERIFY2(a == b, qPrintable(key + QStringLiteral(" vector seed mismatch")));
            const QImage ba = renderKey(key, path, 10, 220, 48, true);
            const QImage bb = renderKey(key, path, 10, 220, 48, true);
            QVERIFY2(ba == bb, qPrintable(key + QStringLiteral(" bitmap seed mismatch")));
        }
        // Different object seeds must be allowed to differ (glitter / lightning)
        stroke::PaintInput in;
        in.params = stroke::paramsFor(QStringLiteral("lw.glitter"));
        in.width = 10;
        in.color = QColor(20, 20, 40);
        auto once = [&](quint32 seed) {
            QImage img(220, 48, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            in.seed = seed;
            stroke::paint(p, path, in);
            return img;
        };
        QVERIFY(once(1) == once(1));
        QVERIFY(once(1) != once(2));
    }

    void calligraphyWidthDependsOnDirection() {
        // A 45-degree flat nib is broad on axis-aligned strokes and thin along the nib.
        auto stroke = [](QPointF a, QPointF b) {
            QImage img(160, 160, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.moveTo(a);
            path.lineTo(b);
            stroke::PaintInput in;
            in.params = stroke::paramsFor(QStringLiteral("calligraphy.basic"));
            in.width = 28;
            in.color = Qt::black;
            in.seed = 1;
            stroke::paint(p, path, in);
            return img;
        };
        const QImage horiz = stroke({20, 80}, {140, 80});
        const QImage diag = stroke({30, 30}, {130, 130}); // along the −45° nib
        const int ht = columnThickness(horiz, 80);
        // thickness of the diagonal: count pixels along a normal near the midpoint
        int dt = 0;
        for (int i = -40; i <= 40; ++i) {
            const int x = 80 + i, y = 80 - i; // normal to (1,1)
            if (x >= 0 && y >= 0 && x < 160 && y < 160 && qAlpha(diag.pixel(x, y)) > 32) ++dt;
        }
        QVERIFY2(ht > dt * 1.35 && ht >= 16 && dt >= 3,
                 qPrintable(QStringLiteral("horiz=%1 diag=%2 (expected horiz much wider)").arg(ht).arg(dt)));
        // Bitmap brush, same preset
        brush::Params bp;
        bp.category = brush::Category::Calligraphy;
        brush::applyTip(bp, brush::Category::Calligraphy, brush::tipIndex(brush::Category::Calligraphy, QStringLiteral("Basic")));
        QCOMPARE(bp.nibRatio, 0.22);
        bp.size = 28;
        bp.color = Qt::black;
        auto brushLine = [&](QPointF a, QPointF b) {
            QImage img(160, 160, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            brush::Stroke s;
            s.begin(img, bp);
            s.composite(img, s.segment(a, a));
            s.composite(img, s.segment(a, b));
            s.composite(img, s.finish());
            s.end();
            return img;
        };
        const QImage bh = brushLine({20, 80}, {140, 80});
        const QImage bd = brushLine({30, 30}, {130, 130});
        const int bht = columnThickness(bh, 80);
        int bdt = 0;
        for (int i = -40; i <= 40; ++i) {
            const int x = 80 + i, y = 80 - i;
            if (x >= 0 && y >= 0 && x < 160 && y < 160 && qAlpha(bd.pixel(x, y)) > 32) ++bdt;
        }
        QVERIFY2(bht > bdt * 1.25 && bht >= 12,
                 qPrintable(QStringLiteral("brush horiz=%1 diag=%2").arg(bht).arg(bdt)));
    }

    void lwdRoundTripCustomStroke() {
        std::unique_ptr<Document> d(Document::createNew(80, 80, Qt::white));
        VObject r;
        r.id = d->newId();
        r.type = ObjType::Rect;
        r.rect = QRectF(10, 10, 50, 50);
        r.strokeKind = QStringLiteral("calligraphy.bamboo");
        r.strokeParams = stroke::paramsFor(r.strokeKind);
        r.strokeParams.inkAmount = 70;
        r.strokeParams.angle = -30;
        r.strokeCustom = true;
        r.strokeWidth = 6;
        d->addObject(r, 0, false);
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(e);
        const VObject &a = e->objects(0)[0];
        QCOMPARE(a.strokeKind, QStringLiteral("calligraphy.bamboo"));
        QVERIFY(a.strokeCustom);
        QCOMPARE(a.strokeParams.inkAmount, 70);
        QCOMPARE(a.strokeParams.angle, -30.0);
    }

    void editStrokeDialogSmoke() {
        EditStrokeDialog dlg;
        dlg.setParams(stroke::paramsFor(QStringLiteral("calligraphy.wet")), QColor(40, 20, 10), 12);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QCOMPARE(dlg.params().name, QStringLiteral("Wet"));
        QVERIFY(dlg.findChild<QTabWidget *>());
    }

    void everyPresetDrawsAndContactSheet() {
        const auto presets = stroke::presets();
        auto ink = [](const QImage &im) {
            int n = 0;
            for (int yy = 0; yy < im.height(); ++yy)
                for (int xx = 0; xx < im.width(); ++xx)
                    if (qAlpha(im.pixel(xx, yy)) > 20) ++n;
            return n;
        };
        auto scurve = [](int W, int H) {
            QPainterPath path;
            path.moveTo(16, H * 0.22);
            path.cubicTo(W * 0.18, H * 0.22, W * 0.32, H * 0.82, W * 0.50, H * 0.82);
            path.cubicTo(W * 0.68, H * 0.82, W * 0.82, H * 0.22, W - 16, H * 0.22);
            return path;
        };
        const QColor black(18, 18, 22);
        const QColor accent(28, 92, 196);
        const QString dir = QStringLiteral("/workspace/brushes-0.3.20");
        QDir().mkpath(dir);
        const QString sheetDir = dir + QStringLiteral("/sheets");
        QDir().mkpath(sheetDir);
        const QString goldDir = dir + QStringLiteral("/golden");
        QDir().mkpath(goldDir);

        // Combined index (compact) plus per-category readable sheets (~1200 px, 30 px S-curves).
        const int idxCellW = 220, idxCellH = 56;
        QImage index(20 + 160 + 2 * idxCellW, 20 + (presets.size() + 1) * idxCellH, QImage::Format_ARGB32_Premultiplied);
        index.fill(Qt::white);
        QPainter ip(&index);
        ip.setPen(QColor(40, 40, 40));
        ip.setFont(QFont(QStringLiteral("Sans"), 9));
        ip.drawText(20, 16, QStringLiteral("Libreworks 0.3.20 stroke presets — vector (left) and bitmap Brush (right)"));
        QPainterPath idxPath = scurve(idxCellW - 4, idxCellH - 10);
        int fail = 0;
        QString failKeys;
        for (int i = 0; i < presets.size(); ++i) {
            const auto &pr = presets[i];
            const int y = 24 + i * idxCellH;
            ip.setPen(QColor(40, 40, 40));
            ip.drawText(20, y + 22, pr.category + QStringLiteral(" / ") + pr.name);
            const QImage v = renderKey(pr.key, idxPath, 10, idxCellW, idxCellH - 8, false, black);
            const QImage b = renderKey(pr.key, idxPath, 10, idxCellW, idxCellH - 8, true, black);
            if (ink(v) < 8) { ++fail; failKeys += pr.key + QStringLiteral("(v) "); }
            if (ink(b) < 2) { ++fail; failKeys += pr.key + QStringLiteral("(b) "); }
            ip.drawImage(180, y, v);
            ip.drawImage(180 + idxCellW, y, b);
            QVERIFY(v.save(goldDir + QLatin1Char('/') + pr.key + QStringLiteral(".png")));
        }
        ip.end();
        QVERIFY(index.save(dir + QStringLiteral("/presets-sheet.png")));

        const int sheetW = 1200, nameW = 210, margin = 16;
        const int colW = (sheetW - nameW - margin * 2) / 4;
        const double thick = 30;
        const QStringList cats = stroke::categories();
        for (const QString &cat : cats) {
            QVector<stroke::Preset> rows;
            for (const auto &pr : presets) if (pr.category == cat) rows.append(pr);
            // Extra overlap samples on Felt Tip and Watercolor sheets.
            struct Extra { QString label; QString key; bool overlap; };
            QVector<Extra> extras;
            if (cat == QLatin1String("Felt Tip")) {
                extras.append({QStringLiteral("Dark Marker (overlap)"), QStringLiteral("felttip.dark"), true});
                extras.append({QStringLiteral("Highlighter (overlap)"), QStringLiteral("felttip.highlighter"), true});
            }
            if (cat == QLatin1String("Watercolor")) {
                extras.append({QStringLiteral("Heavy (overlap)"), QStringLiteral("watercolor.heavy"), true});
                extras.append({QStringLiteral("Thin (overlap)"), QStringLiteral("watercolor.thin"), true});
            }
            double maxExt = 8;
            for (const auto &pr : rows)
                maxExt = qMax(maxExt, stroke::extent(stroke::paramsFor(pr.key), thick));
            const int cellH = qBound(132, int(thick + 2 * maxExt + 48), 230);
            const int headerH = 52;
            const int totalRows = rows.size() + extras.size();
            QImage sheet(sheetW, headerH + 8 + totalRows * cellH + margin, QImage::Format_ARGB32_Premultiplied);
            sheet.fill(QColor(248, 247, 244));
            QPainter sp(&sheet);
            sp.setRenderHint(QPainter::Antialiasing);
            sp.setPen(QColor(30, 30, 34));
            sp.setFont(QFont(QStringLiteral("Sans"), 16, QFont::DemiBold));
            sp.drawText(margin, 28, cat);
            sp.setFont(QFont(QStringLiteral("Sans"), 8));
            sp.setPen(QColor(80, 80, 86));
            const QStringList heads = {QStringLiteral("Vector · black"), QStringLiteral("Vector · colour"),
                                       QStringLiteral("Bitmap · black"), QStringLiteral("Bitmap · colour")};
            for (int c = 0; c < 4; ++c)
                sp.drawText(QRect(nameW + c * colW, 34, colW, 16), Qt::AlignHCenter, heads[c]);
            const QPainterPath curve = scurve(colW - 8, cellH - 10);
            auto blit = [&](int row, int col, const QImage &im) {
                sp.drawImage(nameW + col * colW + 4, headerH + row * cellH + 4, im);
            };
            auto one = [&](const QString &key, const QPainterPath &path, bool bitmap, const QColor &col,
                           const stroke::Params *custom = nullptr) {
                return renderKey(key, path, thick, colW - 8, cellH - 8, bitmap, col, custom);
            };
            int row = 0;
            for (const auto &pr : rows) {
                sp.setPen(QColor(30, 30, 34));
                sp.setFont(QFont(QStringLiteral("Sans"), 10));
                sp.drawText(QRect(margin, headerH + row * cellH, nameW - 8, cellH),
                            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, pr.name);
                blit(row, 0, one(pr.key, curve, false, black));
                blit(row, 1, one(pr.key, curve, false, accent));
                blit(row, 2, one(pr.key, curve, true, black));
                blit(row, 3, one(pr.key, curve, true, accent));
                ++row;
            }
            auto overlapPath = [&]() {
                QPainterPath a = curve, b;
                const int W = colW - 8, H = cellH - 8;
                b.moveTo(18, H * 0.22);
                b.cubicTo(W * 0.28, H * 0.94, W * 0.72, H * 0.06, W - 18, H * 0.78);
                return QPair<QPainterPath, QPainterPath>(a, b);
            };
            auto renderOverlap = [&](const QString &key, bool bitmap, const QColor &col) {
                const auto paths = overlapPath();
                QImage img(colW - 8, cellH - 8, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);
                const QImage a = renderKey(key, paths.first, thick, img.width(), img.height(), bitmap, col);
                const QImage b = renderKey(key, paths.second, thick, img.width(), img.height(), bitmap, col);
                QPainter p(&img);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.drawImage(0, 0, a);
                p.drawImage(0, 0, b);
                return img;
            };
            for (const Extra &ex : extras) {
                sp.setPen(QColor(30, 30, 34));
                sp.setFont(QFont(QStringLiteral("Sans"), 10));
                sp.drawText(QRect(margin, headerH + row * cellH, nameW - 8, cellH),
                            Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, ex.label);
                blit(row, 0, renderOverlap(ex.key, false, black));
                blit(row, 1, renderOverlap(ex.key, false, accent));
                blit(row, 2, renderOverlap(ex.key, true, black));
                blit(row, 3, renderOverlap(ex.key, true, accent));
                ++row;
            }
            sp.end();
            const QString fn = sheetDir + QLatin1Char('/') + cat + QStringLiteral(".png");
            QVERIFY2(sheet.save(fn), qPrintable(fn));
        }
        QVERIFY2(fail == 0, qPrintable(failKeys));
    }
};

QTEST_MAIN(V051Test)
#include "v051_test.moc"
