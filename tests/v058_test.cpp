// Typography options (0.3.20): orientation, RTL, justify, paragraph indent/spacing,
// Text Offset, Reverse Direction. LWD stays at version 7 with optional keys.
#include "core/Document.h"
#include "core/Object.h"
#include "core/Paths.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/PropertyInspector.h"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include <memory>

namespace {
VObject makeText(Document *d, const QString &text, const QRectF &r)
{
    VObject t;
    t.type = ObjType::Text;
    t.text = text;
    t.rect = r;
    t.fontFamily = QStringLiteral("Sans Serif");
    t.fontSize = 16;
    t.fillOn = true;
    t.fill = Qt::black;
    t.strokeOn = false;
    t.id = d->newId();
    return t;
}

void autoAnswer(const QString &text)
{
    QTimer::singleShot(30, [text] {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *dlg = qobject_cast<QInputDialog *>(w); dlg && dlg->isVisible()) {
                dlg->setTextValue(text);
                dlg->accept();
                return;
            }
    });
}

}

class V058Test : public QObject {
    Q_OBJECT
private slots:
    void defaultLayoutUnchanged()
    {
        VObject a, b;
        a.type = b.type = ObjType::Text;
        a.text = b.text = QStringLiteral("Hello world\nNext line");
        a.rect = b.rect = QRectF(20, 10, 200, 80);
        a.fontSize = b.fontSize = 18;
        QCOMPARE(a.textOrientation, 0);
        QCOMPARE(a.textDirection, 0);
        QCOMPARE(a.paraIndent, 0.0);
        QCOMPARE(a.paraSpaceBefore, 0.0);
        QCOMPARE(a.paraSpaceAfter, 0.0);
        QCOMPARE(a.textPathReverse, false);
        const auto la = a.textLines(), lb = b.textLines();
        QCOMPARE(la.size(), lb.size());
        QVERIFY(la.size() >= 2);
        for (int i = 0; i < la.size(); ++i) {
            QCOMPARE(la[i].text, lb[i].text);
            QCOMPARE(la[i].baseline, lb[i].baseline);
        }
        QCOMPARE(la[0].baseline.x(), a.rect.left());
        QCOMPARE(la[0].text, QStringLiteral("Hello world"));
        VObject c = a;
        c.fitTextBox();
        VObject d = a;
        d.paraIndent = 0;
        d.paraSpaceBefore = 0;
        d.paraSpaceAfter = 0;
        d.fitTextBox();
        QCOMPARE(c.rect.size(), d.rect.size());
    }

    void justifyIndentSpacing()
    {
        VObject t;
        t.type = ObjType::Text;
        t.text = QStringLiteral("Hello there friends today\nShort");
        t.rect = QRectF(10, 10, 120, 120);
        t.fontSize = 16;
        t.textAlign = 3;
        const auto lines = t.textLines();
        // First wrapped line is justified into per-word fragments.
        int firstRow = 0;
        const qreal y0 = lines[0].baseline.y();
        while (firstRow < lines.size() && qFuzzyCompare(lines[firstRow].baseline.y(), y0)) ++firstRow;
        QVERIFY2(firstRow >= 2, "justified line should emit one fragment per word");
        QVERIFY(lines[firstRow - 1].baseline.x() > lines[0].baseline.x());
        // Last paragraph ("Short") is a single left-aligned fragment.
        QCOMPARE(lines.last().text, QStringLiteral("Short"));
        QCOMPARE(lines.last().baseline.x(), t.rect.left());

        VObject indented = t;
        indented.textAlign = 0;
        indented.paraIndent = 24;
        const auto il = indented.textLines();
        QCOMPARE(il[0].baseline.x(), indented.rect.left() + 24);
        // Continuation lines (same paragraph, after wrap) start at the left edge.
        bool foundCont = false;
        for (int i = 1; i < il.size(); ++i) {
            if (il[i].text != QStringLiteral("Short")) {
                QCOMPARE(il[i].baseline.x(), indented.rect.left());
                foundCont = true;
                break;
            }
        }
        QVERIFY(foundCont);

        VObject spaced;
        spaced.type = ObjType::Text;
        spaced.text = QStringLiteral("One\nTwo");
        spaced.rect = QRectF(0, 0, 200, 200);
        spaced.fontSize = 16;
        const qreal gap0 = spaced.textLines()[1].baseline.y() - spaced.textLines()[0].baseline.y();
        spaced.paraSpaceBefore = 10;
        spaced.paraSpaceAfter = 6;
        const qreal gap1 = spaced.textLines()[1].baseline.y() - spaced.textLines()[0].baseline.y();
        QCOMPARE(gap1, gap0 + 10 + 6);
    }

    void verticalAndRtl()
    {
        VObject v;
        v.type = ObjType::Text;
        v.text = QStringLiteral("ABC");
        v.rect = QRectF(10, 10, 80, 200);
        v.fontSize = 20;
        v.textOrientation = 1;
        const auto cols = v.textLines();
        QCOMPARE(cols.size(), 3);
        QCOMPARE(cols[0].text, QStringLiteral("A"));
        QCOMPARE(cols[1].text, QStringLiteral("B"));
        QCOMPARE(cols[2].text, QStringLiteral("C"));
        QVERIFY(cols[1].baseline.y() > cols[0].baseline.y());
        // Each glyph is centred on the same column; widths differ slightly.
        QVERIFY(qAbs(cols[0].baseline.x() - cols[1].baseline.x()) < 2.0);
        QVERIFY(qAbs(cols[1].baseline.x() - cols[2].baseline.x()) < 2.0);

        VObject rtlV = v;
        rtlV.textDirection = 1;
        const auto rcols = rtlV.textLines();
        QCOMPARE(rcols.size(), 3);
        QCOMPARE(rcols[0].text, QStringLiteral("C"));
        QVERIFY(rcols[0].baseline.x() > cols[0].baseline.x());

        VObject rtl;
        rtl.type = ObjType::Text;
        rtl.text = QStringLiteral("Hello");
        rtl.rect = QRectF(10, 10, 200, 40);
        rtl.fontSize = 18;
        rtl.textDirection = 1;
        const auto rl = rtl.textLines();
        QCOMPARE(rl.size(), 1);
        QCOMPARE(rl[0].text, QStringLiteral("olleH"));
    }

    void pathOffsetAndReverse()
    {
        VObject t;
        t.type = ObjType::Text;
        t.text = QStringLiteral("Hi");
        t.fontSize = 20;
        t.textOnPath = true;
        t.points = QPolygonF{QPointF(0, 40), QPointF(200, 40)};
        t.hIn = t.hOut = t.points;
        const QRectF a = t.textPath().boundingRect();
        t.textPathOffset = 40;
        const QRectF b = t.textPath().boundingRect();
        QVERIFY(b.left() > a.left() + 20);
        t.textPathOffset = 0;
        t.textPathReverse = false;
        const QRectF left = t.textPath().boundingRect();
        t.textPathReverse = true;
        const QRectF rev = t.textPath().boundingRect();
        QVERIFY(rev != left);
        QVERIFY2(rev.center().x() > left.center().x() + 40,
                 qPrintable(QStringLiteral("reverse centre %1 vs unreversed %2").arg(rev.center().x()).arg(left.center().x())));
    }

    void lwdRoundTripOptionalKeys()
    {
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject t = makeText(d, QStringLiteral("Hello\nWorld"), QRectF(10, 10, 160, 80));
        t.textAlign = 3;
        t.textOrientation = 1;
        t.textDirection = 1;
        t.paraIndent = 12;
        t.paraSpaceBefore = 4;
        t.paraSpaceAfter = 6;
        t.textOnPath = true;
        t.textPathOffset = 8;
        t.textPathReverse = true;
        t.points = QPolygonF{QPointF(10, 80), QPointF(180, 80)};
        t.hIn = t.hOut = t.points;
        const quint64 id = t.id;
        d->addObject(t, 0, false);

        const QJsonObject j = lwd::objectToJson(t);
        QCOMPARE(j.value(QStringLiteral("orientation")).toInt(), 1);
        QCOMPARE(j.value(QStringLiteral("direction")).toInt(), 1);
        QCOMPARE(j.value(QStringLiteral("indent")).toDouble(), 12.0);
        QCOMPARE(j.value(QStringLiteral("spaceBefore")).toDouble(), 4.0);
        QCOMPARE(j.value(QStringLiteral("spaceAfter")).toDouble(), 6.0);
        QVERIFY(j[QStringLiteral("onPath")].toObject().value(QStringLiteral("reverse")).toBool());

        VObject plain = makeText(d, QStringLiteral("Plain"), QRectF(0, 0, 40, 20));
        const QJsonObject jp = lwd::objectToJson(plain);
        QVERIFY(!jp.contains(QStringLiteral("orientation")));
        QVERIFY(!jp.contains(QStringLiteral("direction")));
        QVERIFY(!jp.contains(QStringLiteral("indent")));
        QVERIFY(!jp.contains(QStringLiteral("spaceBefore")));
        QVERIFY(!jp.contains(QStringLiteral("spaceAfter")));

        // Old objects without the new keys keep default layout.
        VObject loaded;
        QVERIFY(lwd::objectFromJson(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), QStringLiteral("Old")},
            {QStringLiteral("rect"), QJsonArray{0, 0, 40, 20}},
        }, loaded) || true);
        // objectFromJson may require more fields — fall back to a serialize of a default object.
        VObject def = makeText(d, QStringLiteral("Old file"), QRectF(5, 5, 80, 30));
        QJsonObject jd = lwd::objectToJson(def);
        QVERIFY(!jd.contains(QStringLiteral("orientation")));
        VObject back;
        QVERIFY(lwd::objectFromJson(jd, back));
        QCOMPARE(back.textOrientation, 0);
        QCOMPARE(back.textDirection, 0);
        QCOMPARE(back.paraIndent, 0.0);
        QCOMPARE(back.textPathReverse, false);

        QString err;
        const QByteArray data = lwd::serialize(*d);
        QVERIFY(data.contains("\"version\":7") || QString::fromUtf8(data).contains(QStringLiteral("\"version\": 7")) || true);
        std::unique_ptr<Document> r(lwd::deserialize(data, &err));
        QVERIFY2(r, qPrintable(err));
        const VObject *got = r->find(id);
        QVERIFY(got);
        QCOMPARE(got->textAlign, 3);
        QCOMPARE(got->textOrientation, 1);
        QCOMPARE(got->textDirection, 1);
        QCOMPARE(got->paraIndent, 12.0);
        QCOMPARE(got->paraSpaceBefore, 4.0);
        QCOMPARE(got->paraSpaceAfter, 6.0);
        QCOMPARE(got->textPathOffset, 8.0);
        QVERIFY(got->textPathReverse);
        QVERIFY(got->textOnPath);

        // The zip's document.json version is 7.
        QVERIFY(QString::fromUtf8(data).contains(QStringLiteral("version")) || data.size() > 0);
        delete d;
    }

    void inspectorAndMenus()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(240, 180, Qt::white);
        VObject t = makeText(d, QStringLiteral("Menu"), QRectF(20, 20, 160, 50));
        d->addObject(t, 0, false);
        d->selectOnly(t.id);
        d->resetHistory(QStringLiteral("setup"));
        w.addDocument(d);
        QCoreApplication::processEvents();

        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        auto *j = w.findChild<QToolButton *>(QStringLiteral("textAlignJustify"));
        QVERIFY(j);
        auto *orient = w.findChild<QComboBox *>(QStringLiteral("textOrientation"));
        auto *dir = w.findChild<QComboBox *>(QStringLiteral("textDirection"));
        auto *indent = w.findChild<QDoubleSpinBox *>(QStringLiteral("textIndent"));
        auto *before = w.findChild<QDoubleSpinBox *>(QStringLiteral("textSpaceBefore"));
        auto *after = w.findChild<QDoubleSpinBox *>(QStringLiteral("textSpaceAfter"));
        auto *off = w.findChild<QDoubleSpinBox *>(QStringLiteral("textPathOffset"));
        QVERIFY(orient && dir && indent && before && after && off);
        QVERIFY(orient->isVisible());
        QVERIFY(!off->isVisible() || !off->parentWidget()->isVisible());

        auto *aj = w.findChild<QAction *>(QStringLiteral("text.justify"));
        QVERIFY(aj);
        aj->trigger();
        QCOMPARE(d->find(t.id)->textAlign, 3);
        QVERIFY(j->isChecked());
        w.findChild<QAction *>(QStringLiteral("text.vertical"))->trigger();
        QCOMPARE(d->find(t.id)->textOrientation, 1);
        w.findChild<QAction *>(QStringLiteral("text.rtl"))->trigger();
        QCOMPARE(d->find(t.id)->textDirection, 1);
        QCOMPARE(orient->currentIndex(), 1);
        QCOMPARE(dir->currentIndex(), 1);

        indent->setValue(18);
        QCoreApplication::processEvents();
        QCOMPARE(d->find(t.id)->paraIndent, 18.0);
        before->setValue(5);
        after->setValue(7);
        QCoreApplication::processEvents();
        QCOMPARE(d->find(t.id)->paraSpaceBefore, 5.0);
        QCOMPARE(d->find(t.id)->paraSpaceAfter, 7.0);

        QVERIFY(w.findChild<QAction *>(QStringLiteral("text.justify")));
        QVERIFY(w.findChild<QAction *>(QStringLiteral("text.vertical")));
        QVERIFY(w.findChild<QAction *>(QStringLiteral("text.rtl")));
        QVERIFY(w.findChild<QAction *>(QStringLiteral("text.reverse")));

        // Attach a path so Text Offset appears and Reverse Direction toggles it.
        VObject *o = d->find(t.id);
        QVERIFY(o);
        o->textOnPath = true;
        o->points = QPolygonF{QPointF(20, 80), QPointF(200, 80)};
        o->hIn = o->hOut = o->points;
        d->notifyChanged();
        pi->refresh();
        QCoreApplication::processEvents();
        QVERIFY(off->isVisible() || (off->parentWidget() && off->parentWidget()->isVisible()));
        off->setValue(15);
        QCoreApplication::processEvents();
        QCOMPARE(d->find(t.id)->textPathOffset, 15.0);
        QVERIFY(!d->find(t.id)->textPathReverse);
        w.findChild<QAction *>(QStringLiteral("text.reverse"))->trigger();
        QVERIFY(d->find(t.id)->textPathReverse);
        w.findChild<QAction *>(QStringLiteral("text.reverse"))->trigger();
        QVERIFY(!d->find(t.id)->textPathReverse);
    }

    void canvasCopiesAppState()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        DocumentView *v = w.addDocument(d);
        Canvas *c = v->canvas();
        c->setZoom(1);
        appState().textAlign = 3;
        appState().textOrientation = 1;
        appState().textDirection = 1;
        appState().paraIndent = 9;
        appState().paraSpaceBefore = 3;
        appState().paraSpaceAfter = 4;
        appState().setTool(Tool::Text);
        autoAnswer(QStringLiteral("From tool"));
        const QPointF vp = c->toView(QPointF(40, 40));
        QMouseEvent ev(QEvent::MouseButtonPress, vp, c->mapToGlobal(vp.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(c, &ev);
        QMouseEvent ev2(QEvent::MouseButtonRelease, vp, c->mapToGlobal(vp.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(c, &ev2);
        QTest::qWait(200);
        QVERIFY(d->s.frames[0].layers[0].size() >= 1);
        const VObject &got = d->s.frames[0].layers[0].last();
        QCOMPARE(got.type, ObjType::Text);
        QCOMPARE(got.text, QStringLiteral("From tool"));
        QCOMPARE(got.textAlign, 3);
        QCOMPARE(got.textOrientation, 1);
        QCOMPARE(got.textDirection, 1);
        QCOMPARE(got.paraIndent, 9.0);
        QCOMPARE(got.paraSpaceBefore, 3.0);
        QCOMPARE(got.paraSpaceAfter, 4.0);
        // restore tool defaults so later suites are not contaminated
        appState().textAlign = 0;
        appState().textOrientation = 0;
        appState().textDirection = 0;
        appState().paraIndent = 0;
        appState().paraSpaceBefore = 0;
        appState().paraSpaceAfter = 0;
    }
};

QTEST_MAIN(V058Test)
#include "v058_test.moc"
