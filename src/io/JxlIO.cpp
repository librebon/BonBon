#include "Codecs.h"
#ifdef LW_HAVE_JXL
#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#include <vector>
#endif

namespace codec {

#ifdef LW_HAVE_JXL
bool readJxl(const QByteArray &d, ImageSeq &out, QString *err) {
    JxlDecoder *dec = JxlDecoderCreate(nullptr);
    void *runner = JxlThreadParallelRunnerCreate(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    JxlDecoderSetParallelRunner(dec, JxlThreadParallelRunner, runner);
    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, reinterpret_cast<const uint8_t *>(d.constData()), size_t(d.size()));
    JxlDecoderCloseInput(dec);
    JxlPixelFormat pf = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlBasicInfo info{};
    info.orientation = JXL_ORIENT_IDENTITY;   // zero is not a JxlOrientation value
    QImage cur;
    int delay = 100;
    bool ok = false;
    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) { setErr(err, QStringLiteral("JXL: decode error")); break; }
        if (st == JXL_DEC_SUCCESS) { ok = true; break; }
        if (st == JXL_DEC_BASIC_INFO) {
            JxlDecoderGetBasicInfo(dec, &info);
        } else if (st == JXL_DEC_FRAME) {
            JxlFrameHeader fh;
            JxlDecoderGetFrameHeader(dec, &fh);
            if (info.have_animation && info.animation.tps_numerator)
                delay = int(double(fh.duration) * 1000.0 * info.animation.tps_denominator / info.animation.tps_numerator + 0.5);
        } else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            size_t sz = 0;
            JxlDecoderImageOutBufferSize(dec, &pf, &sz);
            cur = QImage(int(info.xsize), int(info.ysize), QImage::Format_RGBA8888);
            if (size_t(cur.sizeInBytes()) < sz) { setErr(err, QStringLiteral("JXL: buffer size mismatch")); break; }
            JxlDecoderSetImageOutBuffer(dec, &pf, cur.bits(), sz);
        } else if (st == JXL_DEC_FULL_IMAGE) {
            out.frames.append({cur.convertToFormat(QImage::Format_ARGB32), delay > 0 ? delay : 100});
        }
    }
    out.loops = info.have_animation ? int(info.animation.num_loops) : 0;
    JxlThreadParallelRunnerDestroy(runner);
    JxlDecoderDestroy(dec);
    return ok && !out.frames.isEmpty();
}

bool writeJxl(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err) {
    const int W = seq.frames[0].image.width(), H = seq.frames[0].image.height();
    const bool anim = seq.frames.size() > 1;
    JxlEncoder *enc = JxlEncoderCreate(nullptr);
    void *runner = JxlThreadParallelRunnerCreate(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());
    JxlEncoderSetParallelRunner(enc, JxlThreadParallelRunner, runner);
    auto fail = [&](const char *what) {
        setErr(err, QStringLiteral("JXL: %1").arg(QLatin1String(what)));
        JxlThreadParallelRunnerDestroy(runner);
        JxlEncoderDestroy(enc);
        return false;
    };
    JxlBasicInfo bi;
    JxlEncoderInitBasicInfo(&bi);
    bi.xsize = uint32_t(W);
    bi.ysize = uint32_t(H);
    bi.bits_per_sample = 8;
    bi.num_color_channels = 3;
    bi.alpha_bits = 8;
    bi.num_extra_channels = 1;
    bi.uses_original_profile = o.lossless ? JXL_TRUE : JXL_FALSE;
    if (anim) {
        bi.have_animation = JXL_TRUE;
        bi.animation.tps_numerator = 1000;
        bi.animation.tps_denominator = 1;
        bi.animation.num_loops = uint32_t(qMax(0, seq.loops));
    }
    if (JxlEncoderSetBasicInfo(enc, &bi) != JXL_ENC_SUCCESS) return fail("basic info rejected");
    JxlColorEncoding ce;
    JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc, &ce) != JXL_ENC_SUCCESS) return fail("colour encoding rejected");
    JxlEncoderFrameSettings *fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
    if (o.lossless) JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    else JxlEncoderSetFrameDistance(fs, JxlEncoderDistanceFromQuality(float(qBound(0, o.quality, 100))));
    JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, qBound(1, o.jxlEffort, 9));
    JxlPixelFormat pf = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    for (const auto &f : seq.frames) {
        QImage im = f.image.convertToFormat(QImage::Format_RGBA8888);
        if (anim) {
            JxlFrameHeader fh;
            JxlEncoderInitFrameHeader(&fh);
            fh.duration = uint32_t(qMax(1, f.delayMs));
            JxlEncoderSetFrameHeader(fs, &fh);
        }
        if (JxlEncoderAddImageFrame(fs, &pf, im.constBits(), size_t(W) * H * 4) != JXL_ENC_SUCCESS) return fail("adding frame failed");
    }
    JxlEncoderCloseInput(enc);
    std::vector<uint8_t> buf(1 << 16);
    uint8_t *next = buf.data();
    size_t avail = buf.size();
    JxlEncoderStatus st;
    while ((st = JxlEncoderProcessOutput(enc, &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT) {
        size_t off = size_t(next - buf.data());
        buf.resize(buf.size() * 2);
        next = buf.data() + off;
        avail = buf.size() - off;
    }
    if (st != JXL_ENC_SUCCESS) return fail("encoding failed");
    out = QByteArray(reinterpret_cast<const char *>(buf.data()), int(next - buf.data()));
    JxlThreadParallelRunnerDestroy(runner);
    JxlEncoderDestroy(enc);
    return true;
}
#else
bool readJxl(const QByteArray &, ImageSeq &, QString *err) { setErr(err, QStringLiteral("JPEG XL support not built")); return false; }
bool writeJxl(QByteArray &, const ImageSeq &, const ExportOptions &, QString *err) { setErr(err, QStringLiteral("JPEG XL support not built")); return false; }
#endif

} // namespace codec
