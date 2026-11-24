#include "Codecs.h"
#include <QThread>
#ifdef LW_HAVE_AVIF
#include <avif/avif.h>
#endif

namespace codec {

#ifdef LW_HAVE_AVIF
bool readAvif(const QByteArray &d, ImageSeq &out, QString *err) {
    avifDecoder *dec = avifDecoderCreate();
    if (!dec) { setErr(err, QStringLiteral("AVIF: out of memory")); return false; }
    dec->maxThreads = qMax(1, QThread::idealThreadCount());
    avifResult r = avifDecoderSetIOMemory(dec, reinterpret_cast<const uint8_t *>(d.constData()), size_t(d.size()));
    if (r == AVIF_RESULT_OK) r = avifDecoderParse(dec);
    if (r != AVIF_RESULT_OK) { setErr(err, QStringLiteral("AVIF: %1").arg(avifResultToString(r))); avifDecoderDestroy(dec); return false; }
    while ((r = avifDecoderNextImage(dec)) == AVIF_RESULT_OK) {
        QImage img(int(dec->image->width), int(dec->image->height), QImage::Format_ARGB32);
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, dec->image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.pixels = img.bits();
        rgb.rowBytes = uint32_t(img.bytesPerLine());
        avifResult cr = avifImageYUVToRGB(dec->image, &rgb);
        if (cr != AVIF_RESULT_OK) { setErr(err, QStringLiteral("AVIF: %1").arg(avifResultToString(cr))); avifDecoderDestroy(dec); return false; }
        int delay = int(dec->imageTiming.duration * 1000.0 + 0.5);
        out.frames.append({img, delay > 0 ? delay : 100});
    }
    out.loops = dec->repetitionCount < 0 ? 0 : dec->repetitionCount + 1;
    avifDecoderDestroy(dec);
    if (out.frames.isEmpty()) { setErr(err, QStringLiteral("AVIF: no frames")); return false; }
    return true;
}

bool writeAvif(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err) {
    const int W = seq.frames[0].image.width(), H = seq.frames[0].image.height();
    avifEncoder *enc = avifEncoderCreate();
    enc->maxThreads = qMax(1, QThread::idealThreadCount());
    enc->speed = qBound(0, o.avifSpeed, 10);
    enc->quality = o.lossless ? AVIF_QUALITY_LOSSLESS : qBound(0, o.quality, 100);
    const int alphaQ = o.avifAlphaQuality < 0 ? o.quality + 10 : o.avifAlphaQuality;
    enc->qualityAlpha = o.lossless ? AVIF_QUALITY_LOSSLESS : qBound(0, alphaQ, 100);
    enc->timescale = 1000;
    enc->repetitionCount = seq.loops <= 0 ? AVIF_REPETITION_COUNT_INFINITE : seq.loops - 1;
    bool ok = true;
    QString msg;
    for (int i = 0; ok && i < seq.frames.size(); ++i) {
        QImage im = seq.frames[i].image.convertToFormat(QImage::Format_ARGB32);
        const int depth = (o.avifDepth == 10 || o.avifDepth == 12) ? o.avifDepth : 8;
        avifPixelFormat yuv = AVIF_PIXEL_FORMAT_YUV420;
        if (o.lossless || o.avifChroma == 0) yuv = AVIF_PIXEL_FORMAT_YUV444;
        else if (o.avifChroma == 1) yuv = AVIF_PIXEL_FORMAT_YUV422;
        avifImage *ai = avifImageCreate(uint32_t(W), uint32_t(H), uint32_t(depth), yuv);
        if (o.lossless) {
            ai->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
            ai->yuvRange = AVIF_RANGE_FULL;
        }
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, ai);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.pixels = im.bits();
        rgb.rowBytes = uint32_t(im.bytesPerLine());
        avifResult r = avifImageRGBToYUV(ai, &rgb);
        if (r == AVIF_RESULT_OK)
            r = avifEncoderAddImage(enc, ai, uint64_t(qMax(1, seq.frames[i].delayMs)),
                                    seq.frames.size() == 1 ? AVIF_ADD_IMAGE_FLAG_SINGLE : AVIF_ADD_IMAGE_FLAG_NONE);
        if (r != AVIF_RESULT_OK) { ok = false; msg = QString::fromUtf8(avifResultToString(r)); }
        avifImageDestroy(ai);
    }
    avifRWData data = AVIF_DATA_EMPTY;
    if (ok) {
        avifResult r = avifEncoderFinish(enc, &data);
        if (r != AVIF_RESULT_OK) { ok = false; msg = QString::fromUtf8(avifResultToString(r)); }
        else out = QByteArray(reinterpret_cast<const char *>(data.data), int(data.size));
    }
    avifRWDataFree(&data);
    avifEncoderDestroy(enc);
    if (!ok) setErr(err, QStringLiteral("AVIF: %1").arg(msg));
    return ok;
}
#else
bool readAvif(const QByteArray &, ImageSeq &, QString *err) { setErr(err, QStringLiteral("AVIF support not built")); return false; }
bool writeAvif(QByteArray &, const ImageSeq &, const ExportOptions &, QString *err) { setErr(err, QStringLiteral("AVIF support not built")); return false; }
#endif

} // namespace codec
