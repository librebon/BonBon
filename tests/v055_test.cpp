// Copy to Frames and Distribute to Frames (0.3.20).
#include "core/Document.h"
#include "ui/Dialogs.h"
#include <QRadioButton>
#include <QSpinBox>
#include <QtTest>

class V055Test : public QObject {
    Q_OBJECT
    static VObject box(Document *d, const QRectF &r, const QColor &c)
    {
        VObject o;
        o.type = ObjType::Rect;
        o.rect = r;
        o.fillOn = true;
        o.fill = c;
        o.strokeOn = false;
        o.id = d->newId();
        return o;
    }
    static int countOn(const Document *d, int frame, int layer = 0)
    {
        if (frame < 0 || frame >= d->s.frames.size()) return -1;
        return d->s.frames[frame].layers.value(layer).size();
    }

private slots:
    void copyToAllOtherFrames()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 80, Qt::white));
        d->addFrame(false);
        d->addFrame(false);
        d->setCurrentFrame(0);
        auto a = box(d.get(), QRectF(4, 4, 10, 10), Qt::red);
        d->addObject(a, 0, true);
        d->resetHistory(QStringLiteral("setup"));
        QCOMPARE(d->s.frames.size(), 3);
        QCOMPARE(countOn(d.get(), 0), 1);
        QCOMPARE(countOn(d.get(), 1), 0);
        QCOMPARE(countOn(d.get(), 2), 0);
        d->copyToFrames({0, 1, 2});
        d->commit(QStringLiteral("Copy to Frames"));
        QCOMPARE(countOn(d.get(), 0), 1);
        QCOMPARE(countOn(d.get(), 1), 1);
        QCOMPARE(countOn(d.get(), 2), 1);
        // Fresh ids on the copies; the original stays selected.
        const quint64 orig = a.id;
        QCOMPARE(d->s.frames[0].layers[0][0].id, orig);
        QVERIFY(d->s.frames[1].layers[0][0].id != orig);
        QVERIFY(d->s.frames[2].layers[0][0].id != orig);
        QVERIFY(d->s.frames[1].layers[0][0].id != d->s.frames[2].layers[0][0].id);
        QCOMPARE(d->s.frames[1].layers[0][0].rect, a.rect);
        QVERIFY(d->canUndo());
        d->undo();
        QCOMPARE(countOn(d.get(), 0), 1);
        QCOMPARE(countOn(d.get(), 1), 0);
        QCOMPARE(countOn(d.get(), 2), 0);
    }

    void copyDialogTargets()
    {
        CopyToFramesDialog dlg(5, 2); // current = frame 3 of 5
        auto *all = dlg.findChild<QRadioButton *>(QStringLiteral("copyFramesAll"));
        auto *prev = dlg.findChild<QRadioButton *>(QStringLiteral("copyFramesPrevious"));
        auto *next = dlg.findChild<QRadioButton *>(QStringLiteral("copyFramesNext"));
        auto *range = dlg.findChild<QRadioButton *>(QStringLiteral("copyFramesRange"));
        auto *from = dlg.findChild<QSpinBox *>(QStringLiteral("copyFramesFrom"));
        auto *to = dlg.findChild<QSpinBox *>(QStringLiteral("copyFramesTo"));
        QVERIFY(all && prev && next && range && from && to);
        all->setChecked(true);
        QCOMPARE(dlg.targets(), (QVector<int>{0, 1, 3, 4}));
        prev->setChecked(true);
        QCOMPARE(dlg.targets(), (QVector<int>{1}));
        next->setChecked(true);
        QCOMPARE(dlg.targets(), (QVector<int>{3}));
        range->setChecked(true);
        from->setValue(2);
        to->setValue(4);
        QCOMPARE(dlg.targets(), (QVector<int>{1, 3})); // 2..4 in 1-based, drop current (index 2)
        // First frame has no Previous; last has no Next.
        CopyToFramesDialog first(3, 0);
        QVERIFY(!first.findChild<QRadioButton *>(QStringLiteral("copyFramesPrevious"))->isEnabled());
        QVERIFY(first.findChild<QRadioButton *>(QStringLiteral("copyFramesNext"))->isEnabled());
        CopyToFramesDialog last(3, 2);
        QVERIFY(last.findChild<QRadioButton *>(QStringLiteral("copyFramesPrevious"))->isEnabled());
        QVERIFY(!last.findChild<QRadioButton *>(QStringLiteral("copyFramesNext"))->isEnabled());
    }

    void distributeAddsFramesAndMoves()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 80, Qt::white));
        auto a = box(d.get(), QRectF(0, 0, 8, 8), Qt::red);
        auto b = box(d.get(), QRectF(10, 0, 8, 8), Qt::green);
        auto c = box(d.get(), QRectF(20, 0, 8, 8), Qt::blue);
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->addObject(c, 0, false);
        d->setSelection({a.id, b.id, c.id});
        d->resetHistory(QStringLiteral("setup"));
        QCOMPARE(d->selection.size(), 3);
        QCOMPARE(d->s.frames.size(), 1);
        d->distributeToFrames();
        d->commit(QStringLiteral("Distribute to Frames"));
        QCOMPARE(d->s.frames.size(), 3);
        QCOMPARE(countOn(d.get(), 0), 1);
        QCOMPARE(countOn(d.get(), 1), 1);
        QCOMPARE(countOn(d.get(), 2), 1);
        QCOMPARE(d->s.frames[0].layers[0][0].id, a.id);
        QCOMPARE(d->s.frames[1].layers[0][0].id, b.id);
        QCOMPARE(d->s.frames[2].layers[0][0].id, c.id);
        QCOMPARE(d->curFrame, 0);
        d->undo();
        QCOMPARE(d->s.frames.size(), 1);
        QCOMPARE(countOn(d.get(), 0), 3);
    }

    void distributeFromMiddleFrame()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 80, Qt::white));
        d->addFrame(false); // now on frame 1, two frames total
        auto a = box(d.get(), QRectF(0, 0, 8, 8), Qt::red);
        auto b = box(d.get(), QRectF(10, 0, 8, 8), Qt::green);
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->setSelection({a.id, b.id});
        QCOMPARE(d->curFrame, 1);
        d->distributeToFrames();
        QCOMPARE(d->s.frames.size(), 3); // start=1, two objects -> frames 0,1,2
        QCOMPARE(countOn(d.get(), 0), 0);
        QCOMPARE(countOn(d.get(), 1), 1);
        QCOMPARE(countOn(d.get(), 2), 1);
        QCOMPARE(d->s.frames[1].layers[0][0].id, a.id);
        QCOMPARE(d->s.frames[2].layers[0][0].id, b.id);
    }
};

QTEST_MAIN(V055Test)
#include "v055_test.moc"
