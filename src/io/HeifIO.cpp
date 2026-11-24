// HEIF / HEIC through libheif: every top-level image becomes a frame (decoded to 8-bit RGBA, alpha
// and HDR/10-bit sources are converted by libheif). Encoding uses the HEVC (x265) plugin when present.
// Bundled builds (AppImage / tarball) ship the codec plugins in <prefix>/lib/libheif/plugins.
#include "Codecs.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#ifdef LW_HAVE_HEIF
#include <libheif/heif.h>
#endif

#include <mutex>

namespace codec {

void heifInitPlugins() {
#ifdef LW_HAVE_HEIF
    static std::once_flag once;   // the Optimize estimate encodes on a worker thread too
    std::call_once(once, [] {
    if (qEnvironmentVariableIsEmpty("LIBHEIF_PLUGIN_PATH") && QCoreApplication::instance()) {
        const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/../lib/libheif/plugins");
        if (QFileInfo(dir).isDir()) qputenv("LIBHEIF_PLUGIN_PATH", QDir(dir).absolutePath().toLocal8Bit());
    }
    heif_init(nullptr);
    });
#endif
}

bool heifCanDecode() {
#ifdef LW_HAVE_HEIF
    heifInitPlugins();
    return heif_have_decoder_for_format(heif_compression_HEVC);
#else
    return false;
#endif
}

bool heifCanEncode() {
#ifdef LW_HAVE_HEIF
    heifInitPlugins();
    return heif_have_encoder_for_format(heif_compression_HEVC);
#else
    return false;
#endif
}

#ifdef LW_HAVE_HEIF
namespace {
template<typename F>
void withHevcEncoder(F &&fn) {
    heifInitPlugins();
    heif_context *c = heif_context_alloc();
    if (!c) return;
    heif_encoder *enc = nullptr;
    if (heif_context_get_encoder_for_format(c, heif_compression_HEVC, &enc).code == heif_error_Ok && enc)
        fn(enc);
    if (enc) heif_encoder_release(enc);
    heif_context_free(c);
}
}
#endif

bool heifEncoderHasParameter(const QString &name) {
#ifdef LW_HAVE_HEIF
    bool ok = false;
    withHevcEncoder([&](heif_encoder *enc) {
        const heif_encoder_parameter *const *ps = heif_encoder_list_parameters(enc);
        for (int i = 0; ps && ps[i]; ++i) {
            if (name.compare(QLatin1String(heif_encoder_parameter_get_name(ps[i])), Qt::CaseInsensitive) == 0) {
                ok = true;
                break;
            }
        }
    });
    return ok;
#else
    Q_UNUSED(name);
    return false;
#endif
}

QStringList heifEncoderParameterNames() {
    QStringList names;
#ifdef LW_HAVE_HEIF
    withHevcEncoder([&](heif_encoder *enc) {
        const heif_encoder_parameter *const *ps = heif_encoder_list_parameters(enc);
        for (int i = 0; ps && ps[i]; ++i)
            names << QString::fromUtf8(heif_encoder_parameter_get_name(ps[i]));
    });
#endif
    return names;
}

QString heifEncoderChromaParameterName() {
    for (const QString &n : heifEncoderParameterNames())
        if (n.contains(QLatin1String("chroma"), Qt::CaseInsensitive)) return n;
    return {};
}

#ifdef LW_HAVE_HEIF
namespace {
void applyHeifEncoderOptions(heif_encoder *enc, const ExportOptions &o) {
    if (!o.heifPreset.isEmpty() && heifEncoderHasParameter(QStringLiteral("preset")))
        heif_encoder_set_parameter(enc, "preset", o.heifPreset.toUtf8().constData());
    if (o.heifChroma < 0) return;
    const QString want = o.heifChroma == 0 ? QStringLiteral("420")
                        : o.heifChroma == 1 ? QStringLiteral("422")
                                            : QStringLiteral("444");
    const QString pname = heifEncoderChromaParameterName();
    if (pname.isEmpty()) return;
    const char *const *vals = nullptr;
    if (heif_encoder_parameter_string_valid_values(enc, pname.toUtf8().constData(), &vals).code || !vals) {
        heif_encoder_set_parameter(enc, pname.toUtf8().constData(), want.toUtf8().constData());
        return;
    }
    for (int v = 0; vals[v]; ++v) {
        const QString s = QString::fromUtf8(vals[v]).toLower().remove(QLatin1Char(':'));
        if (s.contains(want)) {
            heif_encoder_set_parameter(enc, pname.toUtf8().constData(), vals[v]);
            return;
        }
    }
}
}
#endif

#ifdef LW_HAVE_HEIF
namespace {
struct Ctx {
    heif_context *c = heif_context_alloc();
    ~Ctx() { heif_context_free(c); }
};
QString heifErr(const heif_error &e) { return QStringLiteral("HEIF: %1").arg(QString::fromUtf8(e.message ? e.message : "error")); }
heif_error writeCb(heif_context *, const void *data, size_t size, void *user) {
    static_cast<QByteArray *>(user)->append(static_cast<const char *>(data), qsizetype(size));
    return heif_error{heif_error_Ok, heif_suberror_Unspecified, "ok"};
}
}
#endif

bool readHeif(const QByteArray &d, ImageSeq &out, QString *err) {
#ifdef LW_HAVE_HEIF
    heifInitPlugins();
    Ctx ctx;
    heif_error e = heif_context_read_from_memory_without_copy(ctx.c, d.constData(), size_t(d.size()), nullptr);
    if (e.code) { setErr(err, heifErr(e)); return false; }
    const int n = heif_context_get_number_of_top_level_images(ctx.c);
    if (n <= 0) { setErr(err, QStringLiteral("HEIF: no images")); return false; }
    QVector<heif_item_id> ids(n);
    heif_context_get_list_of_top_level_image_IDs(ctx.c, ids.data(), n);
    heif_item_id primary = 0;
    heif_context_get_primary_image_ID(ctx.c, &primary);
    std::stable_partition(ids.begin(), ids.end(), [&](heif_item_id i) { return i == primary; });   // primary first
    out.frames.clear();
    QString lastErr;
    for (heif_item_id id : ids) {
        heif_image_handle *hnd = nullptr;
        e = heif_context_get_image_handle(ctx.c, id, &hnd);
        if (e.code) { lastErr = heifErr(e); continue; }
        heif_image *img = nullptr;
        e = heif_decode_image(hnd, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
        if (e.code) { lastErr = heifErr(e); heif_image_handle_release(hnd); continue; }
        const int w = heif_image_get_width(img, heif_channel_interleaved), h = heif_image_get_height(img, heif_channel_interleaved);
        int stride = 0;
        const uint8_t *px = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
        QImage q(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            const uint8_t *s = px + qint64(y) * stride;
            QRgb *dd = reinterpret_cast<QRgb *>(q.scanLine(y));
            for (int x = 0; x < w; ++x) dd[x] = qRgba(s[x * 4], s[x * 4 + 1], s[x * 4 + 2], s[x * 4 + 3]);
        }
        heif_image_release(img);
        heif_image_handle_release(hnd);
        out.frames.push_back({q, ids.size() > 1 ? 500 : 0});
    }
    if (out.frames.isEmpty()) {
        setErr(err, lastErr.isEmpty() ? QStringLiteral("HEIF: could not decode") : lastErr + (heifCanDecode() ? QString() : QStringLiteral(" (no HEVC decoder plugin)")));
        return false;
    }
    out.width = out.frames[0].image.width(); out.height = out.frames[0].image.height(); out.loops = 0;
    return true;
#else
    Q_UNUSED(d); Q_UNUSED(out);
    setErr(err, QStringLiteral("HEIF support not built (libheif missing)"));
    return false;
#endif
}

bool writeHeif(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err) {
#ifdef LW_HAVE_HEIF
    heifInitPlugins();
    if (seq.frames.isEmpty()) { setErr(err, QStringLiteral("HEIF: nothing to write")); return false; }
    Ctx ctx;
    heif_encoder *enc = nullptr;
    heif_error e = heif_context_get_encoder_for_format(ctx.c, heif_compression_HEVC, &enc);
    if (e.code || !enc) { setErr(err, QStringLiteral("HEIF: no HEVC encoder available (install libheif-plugin-x265)")); return false; }
    if (o.lossless) heif_encoder_set_lossless(enc, 1);
    else heif_encoder_set_lossy_quality(enc, qBound(0, o.quality, 100));
    applyHeifEncoderOptions(enc, o);
    const int count = o.animated ? seq.frames.size() : 1;
    for (int i = 0; i < count; ++i) {
        const QImage src = seq.frames[i].image.convertToFormat(QImage::Format_ARGB32);
        bool translucent = false;   // only write an alpha plane when some pixel needs it
        for (int y = 0; y < src.height() && !translucent; ++y) {
            const QRgb *r = reinterpret_cast<const QRgb *>(src.constScanLine(y));
            for (int x = 0; x < src.width(); ++x) if (qAlpha(r[x]) < 255) { translucent = true; break; }
        }
        const bool alpha = o.alpha && translucent;
        const QImage s = alpha ? src : flatten(src, o.matte);
        const int w = s.width(), h = s.height();
        heif_image *img = nullptr;
        e = heif_image_create(w, h, heif_colorspace_RGB, alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, &img);
        if (e.code) { heif_encoder_release(enc); setErr(err, heifErr(e)); return false; }
        heif_image_add_plane(img, heif_channel_interleaved, w, h, 8);
        int stride = 0;
        uint8_t *px = heif_image_get_plane(img, heif_channel_interleaved, &stride);
        const int bpp = alpha ? 4 : 3;
        for (int y = 0; y < h; ++y) {
            const QRgb *r = reinterpret_cast<const QRgb *>(s.constScanLine(y));
            uint8_t *dd = px + qint64(y) * stride;
            for (int x = 0; x < w; ++x) {
                dd[x * bpp] = uint8_t(qRed(r[x])); dd[x * bpp + 1] = uint8_t(qGreen(r[x])); dd[x * bpp + 2] = uint8_t(qBlue(r[x]));
                if (alpha) dd[x * bpp + 3] = uint8_t(qAlpha(r[x]));
            }
        }
        heif_encoding_options *eo = heif_encoding_options_alloc();
        heif_image_handle *hnd = nullptr;
        e = heif_context_encode_image(ctx.c, img, enc, eo, &hnd);
        heif_encoding_options_free(eo);
        heif_image_release(img);
        if (e.code) { heif_encoder_release(enc); setErr(err, heifErr(e)); return false; }
        if (i == 0) heif_context_set_primary_image(ctx.c, hnd);
        heif_image_handle_release(hnd);
    }
    heif_encoder_release(enc);
    out.clear();
    heif_writer w{};
    w.writer_api_version = 1;
    w.write = writeCb;
    e = heif_context_write(ctx.c, &w, &out);
    if (e.code) { setErr(err, heifErr(e)); return false; }
    return true;
#else
    Q_UNUSED(out); Q_UNUSED(seq); Q_UNUSED(o);
    setErr(err, QStringLiteral("HEIF support not built (libheif missing)"));
    return false;
#endif
}

} // namespace codec
