// 0.3.22: Cellular "Sample colours from underneath" and "Follow existing edges"
// are off by default, optional .lwd keys, and only affect the bitmap Brush.
#include "core/Document.h"
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/BrushEngine.h"
#include "core/Filters.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/EditStrokeDialog.h"
#include "ui/MainWindow.h"
#include "ui/PropertyInspector.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QtTest>

namespace {
QImage underHalfPainted(QSize sz)
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, false);
    const int mid = sz.width() / 2;
    p.fillRect(0, 0, mid, sz.height(), QColor(40, 140, 70));
    p.fillRect(0, 0, mid, sz.height() / 3, QColor(200, 40, 40));
    p.fillRect(0, sz.height() * 2 / 3, mid, sz.height() / 3, QColor(40, 70, 200));
    // a hard vertical edge plus a diagonal so Follow existing edges has structure
    p.setPen(QPen(QColor(250, 220, 40), 3));
    p.drawLine(mid / 2, 0, mid / 2, sz.height());
    p.drawLine(0, 8, mid - 2, sz.height() - 8);
    p.end();
    return img;
}
QImage paintCellularOn(const QImage &under, bool sample, bool follow, quint32 seed, const QColor &col = QColor(20, 80, 200))
{
    QImage target = under.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    stroke::PaintInput in;
    in.params = stroke::paramsFor(QStringLiteral("lw.cellular"));
    in.params.cellSampleUnder = sample;
    in.params.cellFollowEdges = follow;
    in.width = 36;
    in.color = col;
    in.seed = seed;
    in.underImage = under;
    QPainterPath path;
    const double y = under.height() * 0.5;
    path.moveTo(8, y);
    path.lineTo(under.width() - 8, y);
    QPainter p(&target);
    p.setRenderHint(QPainter::Antialiasing, true);
    stroke::paint(p, path, in);
    p.end();
    return target;
}
int opaqueDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return a.width() * a.height();
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) if (pa[x] != pb[x]) ++n;
    }
    return n;
}
}

class V062Test : public QObject {
    Q_OBJECT
private slots:
    void defaultsOffAndOmittedFromJson()
    {
        const auto presets = stroke::presets();
        QVERIFY(presets.size() >= 6);
        for (const auto &pr : presets) {
            if (!pr.key.startsWith(QLatin1String("lw.cellular"))) continue;
            const stroke::Params p = stroke::paramsFor(pr.key);
            QVERIFY2(!p.cellSampleUnder && !p.cellFollowEdges, qPrintable(pr.key));
            const QJsonObject j = stroke::toJson(p);
            QVERIFY2(!j.contains(QStringLiteral("cellSampleUnder")), qPrintable(pr.key));
            QVERIFY2(!j.contains(QStringLiteral("cellFollowEdges")), qPrintable(pr.key));
        }
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.cellular"));
        p.cellSampleUnder = true;
        p.cellFollowEdges = true;
        const QJsonObject on = stroke::toJson(p);
        QCOMPARE(on.value(QStringLiteral("cellSampleUnder")).toBool(), true);
        QCOMPARE(on.value(QStringLiteral("cellFollowEdges")).toBool(), true);
        const stroke::Params back = stroke::fromJson(on, stroke::paramsFor(QStringLiteral("lw.cellular")));
        QVERIFY(back.cellSampleUnder && back.cellFollowEdges);
        // An old file with no keys loads as off.
        const stroke::Params old = stroke::fromJson(QJsonObject{{QStringLiteral("key"), QStringLiteral("lw.cellular")}});
        QVERIFY(!old.cellSampleUnder && !old.cellFollowEdges);
    }

    void defaultPaintUnchangedEvenWithUnderImage()
    {
        const QImage under = underHalfPainted(QSize(180, 90));
        const QImage offNoUnder = [&] {
            QImage t(under.size(), QImage::Format_ARGB32_Premultiplied);
            t.fill(Qt::transparent);
            stroke::PaintInput in;
            in.params = stroke::paramsFor(QStringLiteral("lw.cellular"));
            in.width = 36;
            in.color = QColor(20, 80, 200);
            in.seed = 42;
            QPainterPath path;
            path.moveTo(8, 45);
            path.lineTo(172, 45);
            QPainter p(&t);
            p.setRenderHint(QPainter::Antialiasing, true);
            stroke::paint(p, path, in);
            p.end();
            return t;
        }();
        const QImage offWithUnder = paintCellularOn(under, false, false, 42);
        // Subtract the shared under from offWithUnder by painting on a copy of under —
        // compare only the stroke by painting onto a transparent canvas with underImage set.
        QImage t(under.size(), QImage::Format_ARGB32_Premultiplied);
        t.fill(Qt::transparent);
        stroke::PaintInput in;
        in.params = stroke::paramsFor(QStringLiteral("lw.cellular"));
        in.width = 36;
        in.color = QColor(20, 80, 200);
        in.seed = 42;
        in.underImage = under; // flags still off
        QPainterPath path;
        path.moveTo(8, 45);
        path.lineTo(172, 45);
        QPainter p(&t);
        p.setRenderHint(QPainter::Antialiasing, true);
        stroke::paint(p, path, in);
        p.end();
        QCOMPARE(opaqueDiff(offNoUnder, t), 0);
        Q_UNUSED(offWithUnder);
    }

    void transparentFallbackEqualsDefault()
    {
        QImage empty(180, 90, QImage::Format_ARGB32_Premultiplied);
        empty.fill(Qt::transparent);
        const QImage def = paintCellularOn(empty, false, false, 99);
        // Options on, but the under image is fully transparent: must match default exactly.
        const QImage sample = paintCellularOn(empty, true, false, 99);
        const QImage follow = paintCellularOn(empty, false, true, 99);
        const QImage both = paintCellularOn(empty, true, true, 99);
        QCOMPARE(opaqueDiff(def, sample), 0);
        QCOMPARE(opaqueDiff(def, follow), 0);
        QCOMPARE(opaqueDiff(def, both), 0);
    }

    void sampleAndFollowChangePixelsOverPaintedAreas()
    {
        const QImage under = underHalfPainted(QSize(180, 90));
        QImage base(under.size(), QImage::Format_ARGB32_Premultiplied);
        base.fill(Qt::transparent);
        auto strokeOnly = [&](bool sample, bool follow) {
            QImage t = base;
            stroke::PaintInput in;
            in.params = stroke::paramsFor(QStringLiteral("lw.cellular"));
            in.params.cellSampleUnder = sample;
            in.params.cellFollowEdges = follow;
            in.width = 36;
            in.color = QColor(20, 80, 200);
            in.seed = 7;
            in.underImage = under;
            QPainterPath path;
            path.moveTo(8, 45);
            path.lineTo(172, 45);
            QPainter p(&t);
            p.setRenderHint(QPainter::Antialiasing, true);
            stroke::paint(p, path, in);
            p.end();
            return t;
        };
        const QImage off = strokeOnly(false, false);
        const QImage sample = strokeOnly(true, false);
        const QImage follow = strokeOnly(false, true);
        const QImage both = strokeOnly(true, true);
        QVERIFY2(opaqueDiff(off, sample) > 20, "sample colours should change pixels over the painted half");
        QVERIFY2(opaqueDiff(off, follow) > 5, "follow edges should change seeds or boundaries");
        QVERIFY2(opaqueDiff(off, both) > 20, "both options together should change pixels");
        // The empty (right) half should stay close to the default; allow a little CA bleed at the midline.
        auto rightDiff = [](const QImage &a, const QImage &b) {
            int n = 0;
            const int x0 = a.width() * 3 / 4; // well into the empty half
            for (int y = 0; y < a.height(); ++y)
                for (int x = x0; x < a.width(); ++x)
                    if (a.pixel(x, y) != b.pixel(x, y)) ++n;
            return n;
        };
        QVERIFY2(rightDiff(off, sample) < 8, "transparent fallback should keep the empty half on the default pattern");
    }

    void deterministicGivenPreStrokeSnapshot()
    {
        const QImage under = underHalfPainted(QSize(160, 80));
        const QImage a = paintCellularOn(under, true, true, 123);
        const QImage b = paintCellularOn(under, true, true, 123);
        QCOMPARE(opaqueDiff(a, b), 0);
        const QImage c = paintCellularOn(under, true, true, 124);
        QVERIFY(opaqueDiff(a, c) > 0);
    }

    void oldLwdRoundTripOmitsKeysWhenOff()
    {
        auto *d = Document::createNew(80, 60, Qt::white);
        VObject o;
        o.type = ObjType::Path;
        o.rect = QRectF(4, 4, 60, 40);
        o.strokeOn = true;
        o.strokeKind = QStringLiteral("lw.cellular");
        o.strokeCustom = true;
        o.strokeParams = stroke::paramsFor(QStringLiteral("lw.cellular"));
        o.id = d->newId();
        d->addObject(o);
        d->commit(QStringLiteral("cell"));
        const QByteArray raw = lwd::serialize(*d);
        std::unique_ptr<Document> back(lwd::deserialize(raw));
        QVERIFY(back);
        const VObject *p = nullptr;
        for (int li = 0; li < back->s.layers.size(); ++li)
            for (const VObject &x : back->objects(li)) if (x.type == ObjType::Path) p = &x;
        QVERIFY(p && p->strokeCustom);
        QVERIFY(!p->strokeParams.cellSampleUnder && !p->strokeParams.cellFollowEdges);

        // When on, the keys are written and come back.
        auto *d2 = Document::createNew(80, 60, Qt::white);
        VObject q = o;
        q.id = d2->newId();
        q.strokeParams.cellSampleUnder = true;
        q.strokeParams.cellFollowEdges = true;
        d2->addObject(q);
        d2->commit(QStringLiteral("cell-on"));
        const QByteArray raw2 = lwd::serialize(*d2);
        std::unique_ptr<Document> back2(lwd::deserialize(raw2));
        const VObject *p2 = nullptr;
        for (int li = 0; li < back2->s.layers.size(); ++li)
            for (const VObject &x : back2->objects(li)) if (x.type == ObjType::Path) p2 = &x;
        QVERIFY(p2 && p2->strokeParams.cellSampleUnder && p2->strokeParams.cellFollowEdges);
        delete d;
        delete d2;
    }

    void dialogDisablesOptionsForVectorStrokes()
    {
        EditStrokeDialog dlg;
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.cellular"));
        dlg.setParams(p, QColor(20, 80, 200), 12);
        QCoreApplication::processEvents();
        auto *sample = dlg.findChild<QCheckBox *>();
        QList<QCheckBox *> boxes = dlg.findChildren<QCheckBox *>();
        QCheckBox *sampleBox = nullptr, *followBox = nullptr;
        for (QCheckBox *b : boxes) {
            if (b->text().contains(QLatin1String("underneath"))) sampleBox = b;
            if (b->text().contains(QLatin1String("edges"))) followBox = b;
        }
        QVERIFY(sampleBox && followBox);
        QVERIFY(sampleBox->isEnabled() && followBox->isEnabled());
        QVERIFY(!sampleBox->isChecked() && !followBox->isChecked());
        dlg.setVectorStroke(true);
        QVERIFY(!sampleBox->isEnabled() && !followBox->isEnabled());
        QVERIFY(sampleBox->toolTip().contains(QLatin1String("bitmap Brush")));
        Q_UNUSED(sample);
    }

    void writeCombinationSheet()
    {
        const QImage under = underHalfPainted(QSize(260, 110));
        const struct Row { const char *title; bool sample, follow; } rows[] = {
            {"Both off (default)", false, false},
            {"Sample colours from underneath", true, false},
            {"Follow existing edges", false, true},
            {"Both on", true, true},
        };
        const int pad = 16, labelH = 20, cellW = 260, cellH = 110;
        QImage sheet(pad * 2 + cellW + 8, pad + 36 + 4 * (cellH + labelH + 8), QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor(248, 247, 244));
        QPainter sp(&sheet);
        sp.setPen(QColor(30, 30, 34));
        sp.setFont(QFont(QStringLiteral("Sans"), 11, QFont::DemiBold));
        sp.drawText(QRect(pad, 8, cellW, 24), Qt::AlignVCenter,
                    QStringLiteral("Cellular options  —  left half painted, right half empty"));
        sp.setFont(QFont(QStringLiteral("Sans"), 8));
        int y = 40;
        for (const Row &row : rows) {
            sp.setPen(QColor(40, 40, 46));
            sp.drawText(QRect(pad, y, cellW, labelH), Qt::AlignVCenter, QString::fromUtf8(row.title));
            y += labelH;
            QImage cell = paintCellularOn(under, row.sample, row.follow, 21);
            sp.drawImage(pad, y, cell);
            // midline
            sp.setPen(QPen(QColor(0, 0, 0, 80), 1, Qt::DashLine));
            sp.drawLine(pad + cellW / 2, y, pad + cellW / 2, y + cellH);
            y += cellH + 8;
        }
        sp.end();
        const QString dir = QStringLiteral("/workspace/dist-0.3.22-shots/after");
        QDir().mkpath(dir);
        QVERIFY(sheet.save(dir + QStringLiteral("/cellular-options.png")));
    }
};

QTEST_MAIN(V062Test)
#include "v062_test.moc"
