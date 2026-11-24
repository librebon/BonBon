// v0.3.19: system file-dialog pref, MIME default-app scripts, stamp-on-save,
// instanceCount in symbol frames, breakApart/tween of grouped instances.
// File-type combo must not force a huge minimum width on the Qt dialog.
// v0.3.22: the Files of type popup is capped (dialog width or ~60% of the
// screen), long rows elide in the middle, and each item has a full-text tooltip.
#include "ui/AppState.h"
#include "ui/FileDialogs.h"
#include "ui/PreferencesDialog.h"
#include "ui/MainWindow.h"
#include "core/Document.h"
#include "io/LwdFormat.h"
#include "Brand.h"
#include "io/ImageIO.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QScreen>
#include <QTest>
#include <QtTest>

static void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

class V049 : public QObject {
    Q_OBJECT
private slots:
    void useSystemFileDialogsDefaultsOff() {
        QCOMPARE(Prefs{}.useSystemFileDialogs, false);
        Prefs saved = appState().prefs;
        Prefs p = saved;
        p.useSystemFileDialogs = false;
        appState().setPrefs(p);
        QVERIFY(filedialogs::options() & QFileDialog::DontUseNativeDialog);
        QVERIFY(filedialogs::options(QFileDialog::ShowDirsOnly) & QFileDialog::DontUseNativeDialog);
        QVERIFY(filedialogs::options(QFileDialog::ShowDirsOnly) & QFileDialog::ShowDirsOnly);
        p.useSystemFileDialogs = true;
        appState().setPrefs(p);
        QVERIFY(!(filedialogs::options() & QFileDialog::DontUseNativeDialog));
        QVERIFY(filedialogs::options(QFileDialog::ShowDirsOnly) & QFileDialog::ShowDirsOnly);
        QVERIFY(!(filedialogs::options(QFileDialog::ShowDirsOnly) & QFileDialog::DontUseNativeDialog));
        appState().setPrefs(saved);
    }

    void fileDialogPrefCheckboxLiveAndCancel() {
        Prefs saved = appState().prefs;
        Prefs p = saved;
        p.useSystemFileDialogs = false;
        appState().setPrefs(p);

        PreferencesDialog dlg(nullptr);
        auto *cb = dlg.findChild<QCheckBox *>(QStringLiteral("prefUseSystemFileDialogs"));
        QVERIFY2(cb, "expected Use system file dialogs checkbox");
        QVERIFY(!cb->isChecked());
        QCOMPARE(appState().prefs.useSystemFileDialogs, false);
        cb->setChecked(true);
        drainEvents();
        QVERIFY(appState().prefs.useSystemFileDialogs);
        QVERIFY(!(filedialogs::options() & QFileDialog::DontUseNativeDialog));
        dlg.reject();
        drainEvents();
        QVERIFY(!appState().prefs.useSystemFileDialogs);
        QVERIFY(filedialogs::options() & QFileDialog::DontUseNativeDialog);
        appState().setPrefs(saved);
    }

    void mimeappsScriptsUpsertAndLeaveOtherKeys() {
        QTemporaryDir dir;
        const QString target = dir.filePath(QStringLiteral("mimeapps.list"));
        QFile seed(target);
        QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Text));
        seed.write("[Default Applications]\n");
        seed.write("text/plain=gedit.desktop;\n");
        seed.write("[Added Associations]\n");
        seed.write("application/x-libreworks=foo.desktop;\n");
        seed.write("application/pdf=evince.desktop;\n");
        seed.close();

        const QString script = QStringLiteral(LW_SOURCE_DIR) + QStringLiteral("/packaging/debian/postinst");
        QVERIFY(QFile::exists(script));
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("MIMEAPPS_LIST"), target);
        proc.setProcessEnvironment(env);
        proc.start(QStringLiteral("sh"), {script});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 0);

        QFile out(target);
        QVERIFY(out.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray body = out.readAll();
        out.close();
        QVERIFY2(body.contains("text/plain=gedit.desktop;"), body.constData());
        QVERIFY2(body.contains("application/x-libreworks=libreworks.desktop;bonbon.desktop;"), body.constData());
        QVERIFY2(body.contains("application/x-bonbon=bonbon.desktop;libreworks.desktop;"), body.constData());
        QVERIFY2(body.contains("application/x-libreworks=foo.desktop;"), body.constData());
        QVERIFY2(body.contains("application/pdf=evince.desktop;"), body.constData());
        QVERIFY2(body.contains("[Added Associations]"), body.constData());

        // idempotent
        proc.start(QStringLiteral("sh"), {script});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 0);
        QVERIFY(out.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray body2 = out.readAll();
        out.close();
        QCOMPARE(body2.count("application/x-libreworks="), 2);
        QCOMPARE(body2.count("application/x-bonbon="), 1);
        QVERIFY2(body2.contains("application/x-libreworks=foo.desktop;"), body2.constData());

        // prerm must drop only the Default Applications keys, not Added Associations
        const QString prerm = QStringLiteral(LW_SOURCE_DIR) + QStringLiteral("/packaging/debian/prerm");
        QVERIFY(QFile::exists(prerm));
        QProcessEnvironment envRm = env;
        envRm.remove(QStringLiteral("DPKG_MAINTSCRIPT_PACKAGE"));
        proc.setProcessEnvironment(envRm);
        proc.start(QStringLiteral("sh"), {prerm});
        QVERIFY(proc.waitForFinished(5000));
        QCOMPARE(proc.exitCode(), 0);
        QVERIFY(out.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray body3 = out.readAll();
        out.close();
        QVERIFY2(!body3.contains("application/x-libreworks=libreworks.desktop;bonbon.desktop;"), body3.constData());
        QVERIFY2(!body3.contains("application/x-bonbon=bonbon.desktop;libreworks.desktop;"), body3.constData());
        QVERIFY2(body3.contains("application/x-libreworks=foo.desktop;"), body3.constData());
        QVERIFY2(body3.contains("application/pdf=evince.desktop;"), body3.constData());
        QVERIFY2(body3.contains("text/plain=gedit.desktop;"), body3.constData());

        // template matches
        QFile tmpl(QStringLiteral(LW_SOURCE_DIR) + QStringLiteral("/packaging/templates/mimeapps-defaults.list"));
        QVERIFY(tmpl.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray t = tmpl.readAll();
        QVERIFY(t.contains("application/x-libreworks=libreworks.desktop;bonbon.desktop;"));
        QVERIFY(t.contains("application/x-bonbon=bonbon.desktop;libreworks.desktop;"));
    }

    void bonbonMimeUsesLibreworksIconForLwd() {
        const QString cmake = QStringLiteral(LW_SOURCE_DIR) + QStringLiteral("/cmake/LwBrand.cmake");
        QFile f(cmake);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray t = f.readAll();
        // Bonbon's extra application/x-libreworks block must ship the Libreworks icon.
        const int extra = t.indexOf("LW_DOC_MIME_EXTRA");
        QVERIFY(extra >= 0);
        const int endExtra = t.indexOf("LW_ABOUT_TAGLINE", extra);
        const QByteArray block = t.mid(extra, (endExtra > extra ? endExtra : t.size()) - extra);
        QVERIFY2(block.contains("<icon name=\"libreworks\"/>"), block.constData());
        QVERIFY2(!block.contains("<icon name=\"application-x-bonbon\"/>"), block.constData());
        QVERIFY2(block.contains("*.lwd"), block.constData());
    }

    void saveDoesNotStampLiveUnstampedBitmap() {
        Document *d = Document::createNew(32, 24, Qt::white);
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("unstamped");
        o.rect = QRectF(0, 0, 8, 8);
        o.image = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
        o.image.fill(QColor(10, 20, 200));
        o.interpolation = -1;
        d->s.frames[0].layers[0].append(o);
        QCOMPARE(d->s.frames[0].layers[0].last().interpolation, -1);

        const QByteArray bytes = lwd::serialize(*d);
        QVERIFY(!bytes.isEmpty());
        QCOMPARE(d->s.frames[0].layers[0].last().interpolation, -1);

        QString err;
        std::unique_ptr<Document> loaded(lwd::deserialize(bytes, &err));
        QVERIFY2(loaded, qPrintable(err));
        // preview.png is present in the zip (thumbnail path ran)
        QVERIFY(bytes.contains("preview.png"));
        delete d;
    }

    void instanceCountIncludesSymbolFrames() {
        Document *d = Document::createNew(80, 60, Qt::white);
        VObject body;
        body.id = d->newId();
        body.type = ObjType::Rect;
        body.rect = QRectF(0, 0, 20, 20);
        Symbol inner;
        inner.name = QStringLiteral("Inner");
        inner.frames = {{body}};
        const quint64 innerId = d->addSymbol(inner);
        QVERIFY(innerId);

        VObject inst;
        inst.id = d->newId();
        inst.type = ObjType::Instance;
        inst.symbolId = innerId;
        inst.rect = QRectF(0, 0, 20, 20);
        Symbol outer;
        outer.name = QStringLiteral("Outer");
        outer.frames = {{inst}};
        const quint64 outerId = d->addSymbol(outer);
        QVERIFY(outerId);
        QCOMPARE(d->instanceCount(innerId), 1);

        // also count a document-frame instance
        VObject instDoc;
        instDoc.id = d->newId();
        instDoc.type = ObjType::Instance;
        instDoc.symbolId = innerId;
        instDoc.rect = QRectF(10, 10, 20, 20);
        d->s.frames[0].layers[0].append(instDoc);
        QCOMPARE(d->instanceCount(innerId), 2);
        Q_UNUSED(outerId);
        delete d;
    }

    void breakApartAndTweenGroupedInstances() {
        Document *d = Document::createNew(200, 160, Qt::white);
        VObject body;
        body.id = d->newId();
        body.type = ObjType::Rect;
        body.rect = QRectF(0, 0, 20, 20);
        body.fill = Qt::red;
        Symbol sym;
        sym.name = QStringLiteral("Box");
        sym.frames = {{body}};
        const quint64 sid = d->addSymbol(sym);
        QVERIFY(sid);

        VObject a, b;
        a.id = d->newId();
        a.type = ObjType::Instance;
        a.symbolId = sid;
        a.rect = QRectF(10, 10, 20, 20);
        b.id = d->newId();
        b.type = ObjType::Instance;
        b.symbolId = sid;
        b.rect = QRectF(80, 10, 20, 20);
        VObject grp;
        grp.id = d->newId();
        grp.type = ObjType::Group;
        grp.children << a << b;
        d->s.frames[0].layers[0].append(grp);
        const quint64 gid = grp.id, aid = a.id, bid = b.id;

        d->selection = {aid};
        const QVector<quint64> parts = d->breakApart();
        QVERIFY2(!parts.isEmpty(), "breakApart of a grouped instance should yield parts");
        VObject *g = d->find(gid);
        QVERIFY2(g && g->type == ObjType::Group, "group should remain");
        bool stillInst = false, hasPart = false;
        for (const VObject &c : g->children) {
            if (c.id == aid && c.type == ObjType::Instance) stillInst = true;
            if (parts.contains(c.id)) hasPart = true;
        }
        QVERIFY(!stillInst);
        QVERIFY(hasPart);
        // sibling instance survives
        bool hasB = false;
        for (const VObject &c : g->children) if (c.id == bid && c.type == ObjType::Instance) hasB = true;
        QVERIFY(hasB);

        // fresh pair for tween: two instances in a group
        Document *d2 = Document::createNew(200, 160, Qt::white);
        Symbol sym2;
        VObject body2;
        body2.id = d2->newId();
        body2.type = ObjType::Rect;
        body2.rect = QRectF(0, 0, 16, 16);
        sym2.name = QStringLiteral("Box2");
        sym2.frames = {{body2}};
        const quint64 sid2 = d2->addSymbol(sym2);
        VObject ia, ib, g2;
        ia.id = d2->newId();
        ia.type = ObjType::Instance;
        ia.symbolId = sid2;
        ia.rect = QRectF(0, 0, 16, 16);
        ib.id = d2->newId();
        ib.type = ObjType::Instance;
        ib.symbolId = sid2;
        ib.rect = QRectF(60, 0, 16, 16);
        g2.id = d2->newId();
        g2.type = ObjType::Group;
        g2.children << ia << ib;
        d2->s.frames[0].layers[0].append(g2);
        d2->selection = {ia.id, ib.id};
        QVERIFY2(d2->tweenInstances(2, false), "tween of two grouped instances should succeed");
        VObject *gg = d2->find(g2.id);
        QVERIFY(gg);
        int insts = 0;
        for (const VObject &c : gg->children) if (c.type == ObjType::Instance) ++insts;
        QVERIFY2(insts >= 4, "two originals plus two intermediates");
        delete d;
        delete d2;
    }

    void qtFileDialogMinWidthIgnoresLongFilter() {
        Prefs saved = appState().prefs;
        Prefs p = saved;
        p.useSystemFileDialogs = false;
        appState().setPrefs(p);
        QVERIFY(filedialogs::options() & QFileDialog::DontUseNativeDialog);

        const QString filt = ImageIO::importFilter();
        QFileDialog dlg(nullptr, QStringLiteral("Open"), QDir::homePath(), filt);
        dlg.setOption(QFileDialog::DontUseNativeDialog);
        dlg.setAcceptMode(QFileDialog::AcceptOpen);
        dlg.setFileMode(QFileDialog::ExistingFiles);
        filedialogs::configureFilterCombo(&dlg);

        auto *combo = dlg.findChild<QComboBox *>(QStringLiteral("fileTypeCombo"));
        QVERIFY2(combo, "expected QFileDialog fileTypeCombo");
        QCOMPARE(combo->sizeAdjustPolicy(), QComboBox::AdjustToMinimumContentsLengthWithIcon);
        QVERIFY(combo->minimumContentsLength() >= 10);

        int longest = 0;
        QFontMetrics fm(combo->font());
        for (int i = 0; i < combo->count(); ++i)
            longest = qMax(longest, fm.horizontalAdvance(combo->itemText(i)));
        QVERIFY2(longest > 500,
                 qPrintable(QStringLiteral("expected a long All images filter, longest=%1 px items=%2")
                                .arg(longest).arg(combo->count())));

        dlg.show();
        drainEvents();

        const int minHint = dlg.minimumSizeHint().width();
        const int opened = dlg.width();
        QVERIFY2(minHint < longest - 50,
                 qPrintable(QStringLiteral("minimumSizeHint %1 should be well under the longest filter text %2")
                                .arg(minHint).arg(longest)));
        QVERIFY2(opened < longest - 50,
                 qPrintable(QStringLiteral("opened width %1 should not track the longest filter text %2")
                                .arg(opened).arg(longest)));

        dlg.resize(500, dlg.height());
        drainEvents();
        QVERIFY2(dlg.width() <= 520,
                 qPrintable(QStringLiteral("dialog should shrink freely to ~500 px, got %1 (minHint=%2 longest=%3)")
                                .arg(dlg.width()).arg(minHint).arg(longest)));

        dlg.close();
        appState().setPrefs(saved);
    }

    void qtFileDialogFilterPopupCappedElidesAndTooltips() {
        Prefs saved = appState().prefs;
        Prefs p = saved;
        p.useSystemFileDialogs = false;
        appState().setPrefs(p);

        const QString filt = ImageIO::importFilter();
        // Same construction order as production: unfiltered, then options, then filters.
        QFileDialog dlg(nullptr, QStringLiteral("Open"), QDir::homePath());
        dlg.setOption(QFileDialog::DontUseNativeDialog);
        dlg.setAcceptMode(QFileDialog::AcceptOpen);
        dlg.setFileMode(QFileDialog::ExistingFile);
        filedialogs::configureFilterCombo(&dlg);
        dlg.setNameFilter(filt);
        filedialogs::configureFilterCombo(&dlg);

        auto *combo = dlg.findChild<QComboBox *>(QStringLiteral("fileTypeCombo"));
        QVERIFY2(combo, "expected QFileDialog fileTypeCombo");
        auto *view = combo->view();
        QVERIFY2(view, "expected fileTypeCombo view");
        QCOMPARE(view->textElideMode(), Qt::ElideMiddle);

        QVERIFY2(combo->count() >= 5,
                 qPrintable(QStringLiteral("expected the full import filter list, got %1 items")
                                .arg(combo->count())));
        QVERIFY(combo->itemText(0).startsWith(QStringLiteral("All images")));

        int longest = 0;
        QFontMetrics fm(combo->font());
        for (int i = 0; i < combo->count(); ++i)
            longest = qMax(longest, fm.horizontalAdvance(combo->itemText(i)));
        QVERIFY2(longest > 500,
                 qPrintable(QStringLiteral("expected a long All images filter, longest=%1 px")
                                .arg(longest)));

        dlg.show();
        drainEvents();

        const int opened = dlg.width();
        QVERIFY2(opened < longest - 50,
                 qPrintable(QStringLiteral("opened width %1 should not track the longest filter text %2")
                                .arg(opened).arg(longest)));

        auto *name = dlg.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
        QVERIFY2(name, "expected QFileDialog fileNameEdit");
        QVERIFY2(combo->width() <= name->width() + 40,
                 qPrintable(QStringLiteral("closed combo %1 should be about the filename field width %2")
                                .arg(combo->width()).arg(name->width())));
        QVERIFY2(combo->width() < longest - 50,
                 qPrintable(QStringLiteral("closed combo %1 should not grow to the filter text %2")
                                .arg(combo->width()).arg(longest)));

        for (int i = 0; i < combo->count(); ++i) {
            QCOMPARE(combo->itemData(i, Qt::ToolTipRole).toString(), combo->itemText(i));
        }

        const int itemCount = combo->count();
        combo->showPopup();
        QTest::qWait(30);
        drainEvents();
        QCOMPARE(combo->count(), itemCount);
        QCOMPARE(view->textElideMode(), Qt::ElideMiddle);
        for (int i = 0; i < combo->count(); ++i) {
            QCOMPARE(combo->itemData(i, Qt::ToolTipRole).toString(), combo->itemText(i));
        }

        int cap = opened;
        if (QScreen *sc = dlg.screen())
            cap = qMin(cap, qRound(double(sc->availableGeometry().width()) * 0.60));
        QWidget *container = view->window();
        const int popW = (container && container != &dlg) ? container->width() : view->width();
        QVERIFY2(popW <= cap + 16,
                 qPrintable(QStringLiteral("popup width %1 should be capped to min(dialog, 60%% screen)=%2 (dialog=%3)")
                                .arg(popW).arg(cap).arg(opened)));
        QVERIFY2(popW > 0, "expected a visible filter popup");

        combo->hidePopup();
        drainEvents();

        // Short lists may be narrower than the dialog, but still tooltip'd.
        dlg.setNameFilter(QStringLiteral("PNG (*.png);;All files (*)"));
        filedialogs::configureFilterCombo(&dlg);
        drainEvents();
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->itemData(0, Qt::ToolTipRole).toString(), combo->itemText(0));
        QCOMPARE(combo->itemData(1, Qt::ToolTipRole).toString(), combo->itemText(1));
        combo->showPopup();
        QTest::qWait(30);
        drainEvents();
        container = view->window();
        const int shortW = (container && container != &dlg) ? container->width() : view->width();
        QVERIFY2(shortW <= cap + 16,
                 qPrintable(QStringLiteral("short-filter popup %1 should still respect cap %2")
                                .arg(shortW).arg(cap)));
        combo->hidePopup();
        drainEvents();

        dlg.resize(500, dlg.height());
        drainEvents();
        QVERIFY2(dlg.width() <= 520,
                 qPrintable(QStringLiteral("dialog should still shrink to ~500 px, got %1")
                                .arg(dlg.width())));

        dlg.close();
        appState().setPrefs(saved);
    }

};

QTEST_MAIN(V049)
#include "v049_test.moc"
