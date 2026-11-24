// Camera RAW through LibRaw: demosaic with the camera white balance, sRGB output, 8 bits.
// Covers CR2/CR3/NEF/ARW/DNG/RAF/ORF/RW2/PEF/SRW and the other formats LibRaw knows. Read-only.
#include "Codecs.h"
#ifdef LW_HAVE_RAW
#include <libraw/libraw.h>
#include <memory>
#endif

namespace codec {

QString rawLibraryVersion() {
#ifdef LW_HAVE_RAW
    return QString::fromLatin1(libraw_version());
#else
    return {};
#endif
}

bool readRaw(const QByteArray &d, ImageSeq &out, QString *err) {
#ifdef LW_HAVE_RAW
    auto rp = std::make_unique<LibRaw>(0);
    rp->imgdata.params.use_camera_wb = 1;
    rp->imgdata.params.output_color = 1;   // sRGB
    rp->imgdata.params.output_bps = 8;
    rp->imgdata.params.user_flip = -1;     // honour the camera orientation
    int r = rp->open_buffer(d.constData(), size_t(d.size()));
    if (r != LIBRAW_SUCCESS) { setErr(err, QStringLiteral("RAW: %1").arg(QString::fromLatin1(libraw_strerror(r)))); return false; }
    if ((r = rp->unpack()) != LIBRAW_SUCCESS) { setErr(err, QStringLiteral("RAW: %1").arg(QString::fromLatin1(libraw_strerror(r)))); return false; }
    if ((r = rp->dcraw_process()) != LIBRAW_SUCCESS) { setErr(err, QStringLiteral("RAW: %1").arg(QString::fromLatin1(libraw_strerror(r)))); return false; }
    libraw_processed_image_t *im = rp->dcraw_make_mem_image(&r);
    if (!im) { setErr(err, QStringLiteral("RAW: %1").arg(QString::fromLatin1(libraw_strerror(r)))); return false; }
    QImage q;
    if (im->type == LIBRAW_IMAGE_BITMAP && im->bits == 8 && (im->colors == 3 || im->colors == 1)) {
        q = QImage(im->width, im->height, QImage::Format_ARGB32);
        for (int y = 0; y < im->height; ++y) {
            const uchar *s = im->data + qint64(y) * im->width * im->colors;
            QRgb *dd = reinterpret_cast<QRgb *>(q.scanLine(y));
            for (int x = 0; x < im->width; ++x)
                dd[x] = im->colors == 3 ? qRgb(s[x * 3], s[x * 3 + 1], s[x * 3 + 2]) : qRgb(s[x], s[x], s[x]);
        }
    } else if (im->type == LIBRAW_IMAGE_JPEG) {
        q.loadFromData(im->data, int(im->data_size), "JPEG");
        q = q.convertToFormat(QImage::Format_ARGB32);
    }
    LibRaw::dcraw_clear_mem(im);
    if (q.isNull()) { setErr(err, QStringLiteral("RAW: unsupported output")); return false; }
    out.frames = {{q, 0}};
    out.width = q.width(); out.height = q.height(); out.loops = 0;
    return true;
#else
    Q_UNUSED(d); Q_UNUSED(out);
    setErr(err, QStringLiteral("Camera RAW support not built (LibRaw missing)"));
    return false;
#endif
}

} // namespace codec
