#include "Codecs.h"
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

namespace codec {

bool readWebp(const QByteArray &d, ImageSeq &out, QString *err) {
    WebPData wd;
    wd.bytes = reinterpret_cast<const uint8_t *>(d.constData());
    wd.size = size_t(d.size());
    WebPAnimDecoderOptions opt;
    WebPAnimDecoderOptionsInit(&opt);
    opt.color_mode = MODE_BGRA; // matches QImage::Format_ARGB32 on little-endian
    opt.use_threads = 1;
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
    if (!dec) { setErr(err, QStringLiteral("WebP: cannot decode")); return false; }
    WebPAnimInfo info;
    WebPAnimDecoderGetInfo(dec, &info);
    int prev = 0;
    while (WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t *buf = nullptr;
        int ts = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) { setErr(err, QStringLiteral("WebP: frame decode failed")); WebPAnimDecoderDelete(dec); return false; }
        QImage img(buf, int(info.canvas_width), int(info.canvas_height), int(info.canvas_width) * 4, QImage::Format_ARGB32);
        out.frames.append({img.copy(), ts - prev > 0 ? ts - prev : 100});
        prev = ts;
    }
    out.loops = int(info.loop_count);
    out.width = int(info.canvas_width);
    out.height = int(info.canvas_height);
    WebPAnimDecoderDelete(dec);
    return !out.frames.isEmpty();
}

static bool importPicture(WebPPicture &pic, const QImage &src) {
    QImage im = src.convertToFormat(QImage::Format_ARGB32);
    WebPPictureInit(&pic);
    pic.use_argb = 1;
    pic.width = im.width();
    pic.height = im.height();
    return WebPPictureImportBGRA(&pic, im.constBits(), int(im.bytesPerLine()));
}

bool writeWebp(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err) {
    WebPConfig cfg;
    if (!WebPConfigInit(&cfg)) { setErr(err, QStringLiteral("WebP: bad library version")); return false; }
    cfg.lossless = o.lossless ? 1 : 0;
    cfg.quality = float(qBound(0, o.quality, 100));
    cfg.method = qBound(0, o.webpEffort, 6);
    cfg.alpha_quality = qBound(0, o.webpAlphaQuality, 100);
    cfg.use_sharp_yuv = o.webpSharpYuv ? 1 : 0;
    if (o.lossless) cfg.exact = o.webpExact ? 1 : 0;
    const int W = seq.frames[0].image.width(), H = seq.frames[0].image.height();

    if (seq.frames.size() == 1) {
        WebPPicture pic;
        if (!importPicture(pic, seq.frames[0].image)) { setErr(err, QStringLiteral("WebP: import failed")); return false; }
        WebPMemoryWriter wr;
        WebPMemoryWriterInit(&wr);
        pic.writer = WebPMemoryWrite;
        pic.custom_ptr = &wr;
        bool ok = WebPEncode(&cfg, &pic);
        if (ok) out = QByteArray(reinterpret_cast<const char *>(wr.mem), int(wr.size));
        else setErr(err, QStringLiteral("WebP: encode error %1").arg(pic.error_code));
        WebPMemoryWriterClear(&wr);
        WebPPictureFree(&pic);
        return ok;
    }

    WebPAnimEncoderOptions eo;
    WebPAnimEncoderOptionsInit(&eo);
    eo.anim_params.loop_count = qMax(0, seq.loops);
    eo.allow_mixed = 0;
    WebPAnimEncoder *enc = WebPAnimEncoderNew(W, H, &eo);
    if (!enc) { setErr(err, QStringLiteral("WebP: cannot create animation encoder")); return false; }
    int ts = 0;
    bool ok = true;
    for (const auto &f : seq.frames) {
        WebPPicture pic;
        if (!importPicture(pic, f.image)) { ok = false; break; }
        ok = WebPAnimEncoderAdd(enc, &pic, ts, &cfg);
        WebPPictureFree(&pic);
        if (!ok) break;
        ts += qMax(1, f.delayMs);
    }
    WebPData data;
    WebPDataInit(&data);
    if (ok) ok = WebPAnimEncoderAdd(enc, nullptr, ts, nullptr) && WebPAnimEncoderAssemble(enc, &data);
    if (ok) out = QByteArray(reinterpret_cast<const char *>(data.bytes), int(data.size));
    else setErr(err, QStringLiteral("WebP: %1").arg(QString::fromUtf8(WebPAnimEncoderGetError(enc))));
    WebPDataClear(&data);
    WebPAnimEncoderDelete(enc);
    return ok;
}

} // namespace codec
