// 0.3.33: Export Preview "Copy to Clipboard" — the clipboard image matches
// the exported pixels for PNG 32, GIF and JPEG.
#include "core/Document.h"
#include "io/ImageIO.h"
#include "ui/Dialogs.h"
#include "ui/DocumentView.h"
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QDialogButtonBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QtTest>

class V070Test : public QObject {
    Q_OBJECT

    static Document *alphaDoc()
    {
        // Transparent canvas with an opaque patch and a semi-transparent one,
        // so PNG 32 keeps alpha and GIF/JPEG show the format's conversion.
        Document *d = Document::createNew(48, 32, QColor(0, 0, 0, 0));
        VObject o;
        o.type = ObjType::Bitmap;
        o.rect = QRectF(0, 0, 48, 32);
        o.image = QImage(48, 32, QImage::Format_ARGB32);
        o.image.fill(qRgba(0, 0, 0, 0));
        {
            QPainter p(&o.image);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(4, 4, 20, 20, QColor(220, 40, 60, 255));
            p.fillRect(24, 8, 16, 16, QColor(40, 80, 220, 128));
        }
        o.id = d->newId();
        d->addObject(o, 0, false);
        return d;
    }

    static bool samePixels(const QImage &a, const QImage &b)
    {
        if (a.size() != b.size()) return false;
        const QImage aa = a.convertToFormat(QImage::Format_ARGB32);
        const QImage bb = b.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < aa.height(); ++y) {
            const auto *pa = reinterpret_cast<const QRgb *>(aa.constScanLine(y));
            const auto *pb = reinterpret_cast<const QRgb *>(bb.constScanLine(y));
            for (int x = 0; x < aa.width(); ++x)
                if (pa[x] != pb[x]) return false;
        }
        return true;
    }

    static QPushButton *copyButton(ExportDialog *dlg)
    {
        return dlg->findChild<QPushButton *>(QStringLiteral("exportCopyClipboard"));
    }

    void checkFormat(Fmt format, int pngType, const char *mime)
    {
        std::unique_ptr<Document> d(alphaDoc());
        d->exportOptions.format = format;
        d->exportOptions.pngType = pngType;
        d->exportOptions.quality = 80;
        d->exportOptions.colors = 64;
        d->exportOptions.dither = true;
        ExportDialog dlg(*d);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QCoreApplication::processEvents();

        QCOMPARE(dlg.options().format, format);
        auto *copy = copyButton(&dlg);
        QVERIFY2(copy, "Copy to Clipboard button is missing");
        QVERIFY(copy->isEnabled());
        auto *exportBtn = dlg.findChild<QPushButton *>(QStringLiteral("exportPreviewExport"));
        QVERIFY(exportBtn);
        // The Copy button sits directly to the right of Export.
        QVERIFY2(copy->x() >= exportBtn->x() + exportBtn->width() - 2,
                 qPrintable(QStringLiteral("Copy is not to the right of Export (export x=%1 w=%2, copy x=%3)")
                                .arg(exportBtn->x()).arg(exportBtn->width()).arg(copy->x())));

        const EncodeResult expect = encodePreview(*d, dlg.options(), d->curFrame);
        QVERIFY2(expect.error.isEmpty(), qPrintable(expect.error));
        QVERIFY(!expect.image.isNull());

        if (QClipboard *cb = QApplication::clipboard())
            cb->clear();
        QTest::mouseClick(copy, Qt::LeftButton);
        QCoreApplication::processEvents();

        QVERIFY(dlg.isVisible()); // stays open after copying
        auto *status = dlg.findChild<QLabel *>(QStringLiteral("exportCopyStatus"));
        QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("Copied"));

        QClipboard *cb = QApplication::clipboard();
        QVERIFY(cb);
        const QMimeData *md = cb->mimeData();
        QVERIFY(md);
        QVERIFY(md->hasImage());
        const QImage clip = qvariant_cast<QImage>(md->imageData());
        QVERIFY(!clip.isNull());
        QVERIFY2(samePixels(clip, expect.image),
                 "clipboard image does not match the exported pixels");
        if (format == Fmt::PNG) {
            // PNG 32 must keep the semi-transparent and fully transparent pixels.
            const QImage a = clip.convertToFormat(QImage::Format_ARGB32);
            QVERIFY(qAlpha(a.pixel(0, 0)) == 0);
            const int midA = qAlpha(a.pixel(32, 16));
            QVERIFY2(midA > 0 && midA < 255, qPrintable(QStringLiteral("expected semi-transparent pixel, alpha=%1").arg(midA)));
        }
        const QByteArray encoded = md->data(QString::fromLatin1(mime));
        QVERIFY2(!encoded.isEmpty(), qPrintable(QStringLiteral("clipboard is missing %1 bytes").arg(QLatin1String(mime))));
        QCOMPARE(encoded, expect.encoded);
        dlg.reject();
    }

private slots:
    void clipboardMatchesExport_png32()
    {
        checkFormat(Fmt::PNG, 0, "image/png");
    }
    void clipboardMatchesExport_gif()
    {
        if (!ImageIO::available(Fmt::GIF)) QSKIP("GIF support is not built in");
        checkFormat(Fmt::GIF, 0, "image/gif");
    }
    void clipboardMatchesExport_jpeg()
    {
        if (!ImageIO::available(Fmt::JPEG)) QSKIP("JPEG support is not built in");
        checkFormat(Fmt::JPEG, 0, "image/jpeg");
    }
    void copyDisabledForCHeader()
    {
        std::unique_ptr<Document> d(alphaDoc());
        d->exportOptions.format = Fmt::CHeader;
        ExportDialog dlg(*d);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QCoreApplication::processEvents();
        QCOMPARE(dlg.options().format, Fmt::CHeader);
        QVERIFY(!ExportDialog::canCopyFormat(Fmt::CHeader));
        auto *copy = copyButton(&dlg);
        QVERIFY(copy);
        QVERIFY(!copy->isEnabled());
        dlg.reject();
    }
};

QTEST_MAIN(V070Test)
#include "v070_test.moc"
