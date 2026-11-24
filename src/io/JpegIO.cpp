#include "Brand.h"
// JPEG writer through the libjpeg API with three back ends:
//   0 Standard  - the system libjpeg-turbo this program links against
//   1 MozJPEG   - mozjpeg 4 (trellis quantisation, optimised progressive scans), private library
//   2 jpegli    - Google jpegli (adaptive quantisation), private library
// The private libraries implement the same jpeg62 ABI as libjpeg-turbo, so they are dlopen()ed with
// RTLD_LOCAL | RTLD_DEEPBIND and driven through a function table; their symbols never mix with the
// system libjpeg that Qt's image plugins use.
#include "Codecs.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QColorSpace>
#include <QFile>
#include <QMutex>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <jpeglib.h>

namespace codec {
namespace {

// mozjpeg extension parameters (jpeglib.h of mozjpeg 4)
constexpr unsigned kJBooleanOptimizeScans = 0x680C061E;
constexpr unsigned kJBooleanTrellisQuant = 0xC5122033;
constexpr unsigned kJBooleanTrellisQuantDc = 0x339D4C0C;
constexpr unsigned kJBooleanOvershootDeringing = 0x3F4BBBF9;
constexpr unsigned kJIntCompressProfile = 0xE9918625;
constexpr unsigned kJIntBaseQuantTblIdx = 0x44492AB1;
constexpr unsigned kJcpMaxCompression = 0x5D083AAD;

struct JpegApi {
    bool ok = false;
    QString version, path;
    struct jpeg_error_mgr *(*std_error)(struct jpeg_error_mgr *) = nullptr;
    void (*create_compress)(j_compress_ptr, int, size_t) = nullptr;
    void (*set_defaults)(j_compress_ptr) = nullptr;
    void (*set_colorspace)(j_compress_ptr, J_COLOR_SPACE) = nullptr;
    void (*set_quality)(j_compress_ptr, int, boolean) = nullptr;
    void (*simple_progression)(j_compress_ptr) = nullptr;
    void (*mem_dest)(j_compress_ptr, unsigned char **, unsigned long *) = nullptr;
    void (*start_compress)(j_compress_ptr, boolean) = nullptr;
    JDIMENSION (*write_scanlines)(j_compress_ptr, JSAMPARRAY, JDIMENSION) = nullptr;
    void (*finish_compress)(j_compress_ptr) = nullptr;
    void (*destroy_compress)(j_compress_ptr) = nullptr;
    void (*write_marker)(j_compress_ptr, int, const JOCTET *, unsigned int) = nullptr;
    void (*write_icc_profile)(j_compress_ptr, const JOCTET *, unsigned int) = nullptr;
    void (*c_set_bool_param)(j_compress_ptr, unsigned, boolean) = nullptr;
    void (*c_set_int_param)(j_compress_ptr, unsigned, int) = nullptr;
    bool (*c_int_param_supported)(j_compress_ptr, unsigned) = nullptr;
};

JpegApi systemApi() {
    JpegApi a;
    a.ok = true;
    a.version = QStringLiteral("libjpeg-turbo (jpeg%1)").arg(JPEG_LIB_VERSION);
    a.std_error = jpeg_std_error;
    a.create_compress = jpeg_CreateCompress;
    a.set_defaults = jpeg_set_defaults;
    a.set_colorspace = jpeg_set_colorspace;
    a.set_quality = jpeg_set_quality;
    a.simple_progression = jpeg_simple_progression;
    a.mem_dest = jpeg_mem_dest;
    a.start_compress = jpeg_start_compress;
    a.write_scanlines = jpeg_write_scanlines;
    a.finish_compress = jpeg_finish_compress;
    a.destroy_compress = jpeg_destroy_compress;
    a.write_marker = jpeg_write_marker;
    a.write_icc_profile = jpeg_write_icc_profile;
    return a;
}

QString exeDir() {
    if (QCoreApplication::instance()) return QCoreApplication::applicationDirPath();
    return QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath().section(QLatin1Char('/'), 0, -2);
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#  define LW_SANITIZER_BUILD 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#    define LW_SANITIZER_BUILD 1
#  endif
#endif

[[maybe_unused]] JpegApi loadApi(const char *file) {
    JpegApi a;
    QStringList dirs;
    const QByteArray env = qgetenv("LW_JPEG_ENCODER_DIR");
    if (!env.isEmpty()) dirs << QString::fromLocal8Bit(env);
    const QString ed = exeDir();
    dirs << ed + QStringLiteral("/../lib/") + brand::nameLower() << ed + QStringLiteral("/jpeg-encoders") << ed + QStringLiteral("/../jpeg-encoders")
         << ed + QStringLiteral("/../../jpeg-encoders");
#ifdef LW_JPEG_ENCODER_BUILD_DIR
    dirs << QStringLiteral(LW_JPEG_ENCODER_BUILD_DIR);
#endif
    void *h = nullptr;
    for (const QString &d : dirs) {
        const QString p = QDir::cleanPath(d + QLatin1Char('/') + QLatin1String(file));
        if (!QFileInfo::exists(p)) continue;
        // ASan/UBSan abort on RTLD_DEEPBIND (google/sanitizers#611); LOCAL alone is enough for tests.
        int dlflags = RTLD_NOW | RTLD_LOCAL;
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__)
#  if defined(__has_feature)
#    if !__has_feature(address_sanitizer) && !__has_feature(undefined_behavior_sanitizer)
        dlflags |= RTLD_DEEPBIND;
#    endif
#  else
        dlflags |= RTLD_DEEPBIND;
#  endif
#endif
        h = dlopen(QFile::encodeName(p).constData(), dlflags);
        if (h) { a.path = p; break; }
    }
    if (!h) return a;
    auto sym = [&](const char *n) { return dlsym(h, n); };
#define LW_SYM(field, name) a.field = reinterpret_cast<decltype(a.field)>(sym(name))
    LW_SYM(std_error, "jpeg_std_error");
    LW_SYM(create_compress, "jpeg_CreateCompress");
    LW_SYM(set_defaults, "jpeg_set_defaults");
    LW_SYM(set_colorspace, "jpeg_set_colorspace");
    LW_SYM(set_quality, "jpeg_set_quality");
    LW_SYM(simple_progression, "jpeg_simple_progression");
    LW_SYM(mem_dest, "jpeg_mem_dest");
    LW_SYM(start_compress, "jpeg_start_compress");
    LW_SYM(write_scanlines, "jpeg_write_scanlines");
    LW_SYM(finish_compress, "jpeg_finish_compress");
    LW_SYM(destroy_compress, "jpeg_destroy_compress");
    LW_SYM(write_marker, "jpeg_write_marker");
    LW_SYM(write_icc_profile, "jpeg_write_icc_profile");
    LW_SYM(c_set_bool_param, "jpeg_c_set_bool_param");
    LW_SYM(c_set_int_param, "jpeg_c_set_int_param");
#undef LW_SYM
    a.ok = a.std_error && a.create_compress && a.set_defaults && a.set_quality && a.simple_progression && a.mem_dest && a.start_compress
           && a.write_scanlines && a.finish_compress && a.destroy_compress && a.write_marker;
    return a;
}

const JpegApi &api(int encoder) {
    static QMutex m;
    static bool loaded[3] = {false, false, false};
    static JpegApi apis[3];
    QMutexLocker lock(&m);
    encoder = qBound(0, encoder, 2);
    if (!loaded[encoder]) {
        loaded[encoder] = true;
        if (encoder == 0) apis[0] = systemApi();
        else if (encoder == 1) {
#ifdef LW_SANITIZER_BUILD
            // Private mozjpeg needs RTLD_DEEPBIND; ASan forbids it, and without it progressive encode can yield 0 bytes.
            apis[1] = JpegApi{};
#else
            apis[1] = loadApi("liblw-mozjpeg.so"); if (apis[1].ok) apis[1].version = QStringLiteral("mozjpeg 4.1.5");
#endif
        } else {
#ifdef LW_SANITIZER_BUILD
            apis[2] = JpegApi{};
#else
            apis[2] = loadApi("liblw-jpegli.so"); if (apis[2].ok) apis[2].version = QStringLiteral("jpegli (libjpeg62 API)");
#endif
        }
    }
    return apis[encoder];
}

struct ErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
};
void onError(j_common_ptr c) {
    ErrMgr *e = reinterpret_cast<ErrMgr *>(c->err);
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jb, 1);
}
void onMessage(j_common_ptr, int) {}

// Fireworks "Sharpen color edges": keep full-resolution chroma and sharpen the chroma planes a little
// so saturated edges (red text on white, ...) survive the DCT without bleeding.
QImage sharpenChroma(const QImage &src) {
    const int w = src.width(), h = src.height();
    QImage out(w, h, QImage::Format_RGB888);
    std::vector<float> Y(size_t(w) * h), Cb(size_t(w) * h), Cr(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const uchar *s = src.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const float r = s[x * 3], g = s[x * 3 + 1], b = s[x * 3 + 2];
            const size_t i = size_t(y) * w + x;
            Y[i] = 0.299f * r + 0.587f * g + 0.114f * b;
            Cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            Cr[i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
    }
    auto sharpen = [&](std::vector<float> &c) {
        std::vector<float> o(c.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float sum = 0; int n = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = qBound(0, x + dx, w - 1), yy = qBound(0, y + dy, h - 1);
                    sum += c[size_t(yy) * w + xx]; ++n;
                }
                o[i] = c[i] + 0.6f * (c[i] - sum / n);
            }
        c.swap(o);
    };
    sharpen(Cb); sharpen(Cr);
    for (int y = 0; y < h; ++y) {
        uchar *d = out.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            auto c8 = [](float v) { return uchar(qBound(0, int(std::lround(v)), 255)); };
            d[x * 3] = c8(Y[i] + 1.402f * Cr[i]);
            d[x * 3 + 1] = c8(Y[i] - 0.344136f * Cb[i] - 0.714136f * Cr[i]);
            d[x * 3 + 2] = c8(Y[i] + 1.772f * Cb[i]);
        }
    }
    return out;
}

bool encode(const JpegApi &a, int encoder, QByteArray &out, const QImage &rgb, const QByteArray &icc, const ExportOptions &o, QString *err) {
    struct jpeg_compress_struct ci;
    ErrMgr em;
    std::memset(&ci, 0, sizeof ci);
    ci.err = a.std_error(&em.pub);
    em.pub.error_exit = onError;
    em.pub.emit_message = onMessage;
    unsigned char *buf = nullptr;
    unsigned long len = 0;
    if (setjmp(em.jb)) {
        setErr(err, QStringLiteral("JPEG (%1): %2").arg(a.version, QString::fromLocal8Bit(em.msg)));
        a.destroy_compress(&ci);
        free(buf);
        return false;
    }
    a.create_compress(&ci, JPEG_LIB_VERSION, sizeof ci);
    a.mem_dest(&ci, &buf, &len);
    ci.image_width = JDIMENSION(rgb.width());
    ci.image_height = JDIMENSION(rgb.height());
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    if (encoder == 1 && a.c_set_int_param) a.c_set_int_param(&ci, kJIntCompressProfile, int(kJcpMaxCompression));
    a.set_defaults(&ci);
    if (encoder == 1 && a.c_set_int_param) {
        a.c_set_int_param(&ci, kJIntBaseQuantTblIdx, 3);   // ImageMagick table, mozjpeg's default
        a.c_set_bool_param(&ci, kJBooleanTrellisQuant, TRUE);
        a.c_set_bool_param(&ci, kJBooleanTrellisQuantDc, TRUE);
        a.c_set_bool_param(&ci, kJBooleanOvershootDeringing, TRUE);
        a.c_set_bool_param(&ci, kJBooleanOptimizeScans, o.jpegProgressive ? TRUE : FALSE);
    }
    a.set_quality(&ci, qBound(1, o.quality, 100), TRUE);
    // chroma subsampling (component 0 = Y)
    const int sub = o.jpegSharpenEdges ? 0 : qBound(0, o.jpegSubsampling, 2);
    ci.comp_info[0].h_samp_factor = sub == 0 ? 1 : 2;
    ci.comp_info[0].v_samp_factor = sub == 2 ? 2 : 1;
    for (int c = 1; c < 3; ++c) { ci.comp_info[c].h_samp_factor = 1; ci.comp_info[c].v_samp_factor = 1; }
    ci.optimize_coding = (o.jpegOptimize || o.jpegProgressive || encoder == 1) ? TRUE : FALSE;
    if (o.jpegProgressive) a.simple_progression(&ci);
    else { ci.num_scans = 0; ci.scan_info = nullptr; }   // mozjpeg's profile turns progressive on by default
    ci.write_JFIF_header = TRUE;
    if (o.dpi > 0) { ci.density_unit = 1; ci.X_density = ci.Y_density = UINT16(qBound(1, qRound(o.dpi), 65535)); }
    a.start_compress(&ci, TRUE);
    if (!o.jpegStripMeta) {
        static const QByteArray kComment = QByteArray("Created with ") + LW_PRODUCT_NAME + " " LW_VERSION;
        a.write_marker(&ci, JPEG_COM, reinterpret_cast<const JOCTET *>(kComment.constData()), unsigned(kComment.size()));
    }
    if (o.jpegKeepIcc && !icc.isEmpty()) {
        if (a.write_icc_profile) a.write_icc_profile(&ci, reinterpret_cast<const JOCTET *>(icc.constData()), unsigned(icc.size()));
    }
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb.constScanLine(int(ci.next_scanline)));
        a.write_scanlines(&ci, &row, 1);
    }
    a.finish_compress(&ci);
    a.destroy_compress(&ci);
    out = QByteArray(reinterpret_cast<const char *>(buf), qsizetype(len));
    free(buf);
    return true;
}
}

bool jpegEncoderAvailable(int encoder) { return api(encoder).ok; }
QString jpegEncoderVersion(int encoder) { return api(encoder).ok ? api(encoder).version : QString(); }

// Fireworks "Smoothing" 1..8: a [1 2 1] blur applied before the DCT, blended in by level / 8 and
// repeated for the higher levels. Done here (not via smoothing_factor) so every encoder honours it.
static QImage smoothImage(const QImage &src, int level) {
    QImage img = src;
    const int w = img.width(), h = img.height();
    const int passes = (level + 2) / 3;                 // 1..3
    const int mix = qBound(0, level * 256 / 8, 256);    // blend weight /256
    for (int pass = 0; pass < passes; ++pass) {
        QImage tmp(w, h, QImage::Format_RGB888), out(w, h, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y) {                    // horizontal
            const uchar *r = img.constScanLine(y); uchar *d = tmp.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int xl = qMax(0, x - 1), xr = qMin(w - 1, x + 1);
                for (int c = 0; c < 3; ++c) d[x * 3 + c] = uchar((r[xl * 3 + c] + 2 * r[x * 3 + c] + r[xr * 3 + c] + 2) / 4);
            }
        }
        for (int y = 0; y < h; ++y) {                    // vertical + blend
            const uchar *u = tmp.constScanLine(qMax(0, y - 1)), *m = tmp.constScanLine(y), *b = tmp.constScanLine(qMin(h - 1, y + 1));
            const uchar *o = img.constScanLine(y); uchar *d = out.scanLine(y);
            for (int i = 0; i < w * 3; ++i) {
                const int bl = (u[i] + 2 * m[i] + b[i] + 2) / 4;
                d[i] = uchar((o[i] * (256 - mix) + bl * mix + 128) >> 8);
            }
        }
        img = out;
    }
    return img;
}

bool writeJpeg(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err) {
    if (img.isNull() || img.width() > 65500 || img.height() > 65500) { setErr(err, QStringLiteral("JPEG: image must be 1..65500 px")); return false; }
    int enc = qBound(0, o.jpegEncoder, 2);
    const JpegApi *a = &api(enc);
    if (!a->ok) { a = &api(0); enc = 0; }   // fall back to libjpeg-turbo when the private library is missing
    QImage rgb = flatten(img, o.matte).convertToFormat(QImage::Format_RGB888);
    if (o.jpegSmoothing > 0) rgb = smoothImage(rgb, qBound(1, o.jpegSmoothing, 8));
    if (o.jpegSharpenEdges) rgb = sharpenChroma(rgb);
    QByteArray icc;
    if (o.jpegKeepIcc && img.colorSpace().isValid() && img.colorSpace() != QColorSpace(QColorSpace::SRgb)) icc = img.colorSpace().iccProfile();
    return encode(*a, enc, out, rgb, icc, o, err);
}

} // namespace codec
