// PNG writer on libpng (exact colour type and bit depth): PNG 8 (indexed, tRNS index or alpha
// transparency), PNG 24, PNG 32, greyscale (8/16), 16-bit, Adam7 interlacing, zlib level 0-9.
// Also the PNG chunk helpers used by the editable (Fireworks-style) PNG and Fireworks detection.
#include "Codecs.h"
#include "Quantize.h"
#include <png.h>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace codec {

namespace {
void pngWriteCb(png_structp p, png_bytep data, png_size_t len) {
    static_cast<QByteArray *>(png_get_io_ptr(p))->append(reinterpret_cast<const char *>(data), qsizetype(len));
}
void pngFlushCb(png_structp) {}
void pngErrorCb(png_structp p, png_const_charp msg) {
    auto *e = static_cast<QString *>(png_get_error_ptr(p));
    if (e) *e = QString::fromLatin1(msg);
    png_longjmp(p, 1);
}
void pngWarnCb(png_structp, png_const_charp) {}

bool hasTransparency(const QImage &img) {
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) if (qAlpha(s[x]) < 255) return true;
    }
    return false;
}

// PNG 8 with alpha transparency: opaque colours from the normal palette builder, translucent
// pixels share up to a quarter of the table (most frequent 4-bit RGBA buckets), index 0 is clear.
struct RgbaPal { QVector<QRgb> pal; QVector<uchar> idx; };
RgbaPal quantizeAlpha(const QImage &img, const ExportOptions &o) {
    const int w = img.width(), h = img.height();
    const int maxColors = qBound(2, o.colors, 256);
    bool anyClear = false;
    std::map<quint32, std::pair<long long, std::array<long long, 4>>> buckets;
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = qAlpha(s[x]);
            if (a == 0) { anyClear = true; continue; }
            if (a == 255) continue;
            const quint32 key = quint32(qRed(s[x]) >> 4) << 12 | quint32(qGreen(s[x]) >> 4) << 8 | quint32(qBlue(s[x]) >> 4) << 4 | quint32(a >> 4);
            auto &b = buckets[key];
            b.first++; b.second[0] += qRed(s[x]); b.second[1] += qGreen(s[x]); b.second[2] += qBlue(s[x]); b.second[3] += a;
        }
    }
    QVector<QRgb> translucent;
    {
        std::vector<std::pair<long long, QRgb>> v;
        for (auto &kv : buckets) {
            const long long n = kv.second.first;
            const auto &sm = kv.second.second;
            v.push_back({n, qRgba(int(sm[0] / n), int(sm[1] / n), int(sm[2] / n), int(sm[3] / n))});
        }
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
        const int limit = buckets.empty() ? 0 : qMax(1, maxColors / 4);
        for (int i = 0; i < int(v.size()) && i < limit; ++i) translucent << v[i].second;
    }
    const int reserved = (anyClear ? 1 : 0) + translucent.size();
    QImage opaque = img.copy();
    for (int y = 0; y < h; ++y) {   // only opaque pixels feed the opaque table
        QRgb *s = reinterpret_cast<QRgb *>(opaque.scanLine(y));
        for (int x = 0; x < w; ++x) if (qAlpha(s[x]) < 255) s[x] = 0;
    }
    QVector<QRgb> op = quant::buildPalette({opaque}, qMax(2, maxColors - reserved), o.palette, o.customPalette);
    if (op.size() > 256 - reserved) op.resize(256 - reserved);
    RgbaPal r;
    if (anyClear) r.pal << qRgba(0, 0, 0, 0);
    r.pal << translucent;
    const int opOff = r.pal.size();
    r.pal << op;
    // opaque pixels: palette mapping with dithering; translucent: nearest RGBA entry
    QVector<uchar> base = quant::mapImage(opaque, op, o.dither ? o.ditherAmount : 0, o.loss, -1, opOff);
    r.idx = base;
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = qAlpha(s[x]);
            if (a == 255) continue;
            if (a == 0 || translucent.isEmpty()) {
                if (anyClear) r.idx[y * w + x] = 0;
                continue;
            }
            int best = 0, bestD = INT32_MAX;
            for (int i = 0; i < translucent.size(); ++i) {
                const QRgb c = translucent[i];
                const int dr = qRed(c) - qRed(s[x]), dg = qGreen(c) - qGreen(s[x]), db = qBlue(c) - qBlue(s[x]), da = qAlpha(c) - a;
                const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2 + da * da * 6;
                if (d < bestD) { bestD = d; best = i; }
            }
            r.idx[y * w + x] = uchar((anyClear ? 1 : 0) + best);
        }
    }
    return r;
}

// Index (or no) transparency: semi-transparent edges are blended onto the matte; alpha < 128 is clear.
QImage matteBlend(const QImage &img, const QColor &matte, bool keepClear) {
    QImage out = img.copy();
    const int mr = matte.red(), mg = matte.green(), mb = matte.blue();
    for (int y = 0; y < out.height(); ++y) {
        QRgb *s = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const int a = qAlpha(s[x]);
            if (a == 255) continue;
            if (keepClear && a < 128) { s[x] = 0; continue; }
            s[x] = qRgb((qRed(s[x]) * a + mr * (255 - a) + 127) / 255, (qGreen(s[x]) * a + mg * (255 - a) + 127) / 255,
                        (qBlue(s[x]) * a + mb * (255 - a) + 127) / 255);
        }
    }
    return out;
}
}

QVector<QRgb> indexedPalette(const QImage &src, const ExportOptions &o) {
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    if (o.format == Fmt::PNG && o.pngType == 2 && o.pngTransparency == 2) return quantizeAlpha(img, o).pal;
    const bool keepClear = o.format == Fmt::GIF || o.pngTransparency == 1;
    const QImage m = matteBlend(img, o.matte, keepClear);
    bool clear = false;
    if (keepClear) for (int y = 0; y < m.height() && !clear; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(m.constScanLine(y));
        for (int x = 0; x < m.width(); ++x) if (qAlpha(s[x]) == 0) { clear = true; break; }
    }
    int maxColors = qBound(2, o.colors, 256);
    if (clear && maxColors == 256) maxColors = 255;
    QVector<QRgb> pal = quant::buildPalette({m}, maxColors, o.palette, o.customPalette);
    if (clear) { if (pal.size() > 255) pal.resize(255); pal.prepend(qRgba(0, 0, 0, 0)); }
    return pal;
}

bool writePng(QByteArray &out, const QImage &src, const ExportOptions &o, QString *err) {
    const QImage img = src.format() == QImage::Format_ARGB32 ? src : src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    const int type = qBound(0, o.pngType, 3);
    const bool b16 = o.png16 && type != 2;
    // volatile: live across setjmp below (-Wclobbered); they are not changed after it
    volatile int colorType = PNG_COLOR_TYPE_RGB_ALPHA, depth = b16 ? 16 : 8;
    QVector<QRgb> pal;
    QVector<uchar> idx;
    QImage pix = img;
    switch (type) {
    case 0: colorType = PNG_COLOR_TYPE_RGB_ALPHA; break;
    case 1: colorType = PNG_COLOR_TYPE_RGB; pix = matteBlend(img, o.matte, false); break;
    case 3: colorType = o.alpha ? PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_GRAY; if (!o.alpha) pix = matteBlend(img, o.matte, false); break;
    case 2: {
        colorType = PNG_COLOR_TYPE_PALETTE;
        if (o.pngTransparency == 2) {
            RgbaPal r = quantizeAlpha(img, o);
            pal = r.pal; idx = r.idx;
        } else {
            const bool keepClear = o.pngTransparency == 1;
            pix = matteBlend(img, o.matte, keepClear);
            bool clear = keepClear && hasTransparency(pix);
            int maxColors = qBound(2, o.colors, 256);
            if (clear && maxColors == 256) maxColors = 255;
            pal = quant::buildPalette({pix}, maxColors, o.palette, o.customPalette);
            if (clear && pal.size() > 255) pal.resize(255);
            idx = quant::mapImage(pix, pal, o.dither ? o.ditherAmount : 0, o.loss, clear ? 0 : -1, clear ? 1 : 0);
            if (clear) pal.prepend(qRgba(0, 0, 0, 0));
        }
        depth = pal.size() <= 2 ? 1 : pal.size() <= 4 ? 2 : pal.size() <= 16 ? 4 : 8;
        break;
    }
    }
    out.clear();
    QString perr;
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, &perr, pngErrorCb, pngWarnCb);
    if (!p) { setErr(err, QStringLiteral("PNG: out of memory")); return false; }
    png_infop info = png_create_info_struct(p);
    if (!info) { png_destroy_write_struct(&p, nullptr); setErr(err, QStringLiteral("PNG: out of memory")); return false; }
    // rows live outside the setjmp scope
    std::vector<std::vector<png_byte>> rows(h);
    std::vector<png_bytep> rowPtrs(h);
    if (setjmp(png_jmpbuf(p))) {
        png_destroy_write_struct(&p, &info);
        setErr(err, QStringLiteral("PNG: %1").arg(perr));
        return false;
    }
    png_set_write_fn(p, &out, pngWriteCb, pngFlushCb);
    png_set_compression_level(p, qBound(0, o.pngCompression, 9));
    png_set_IHDR(p, info, png_uint_32(w), png_uint_32(h), depth, colorType,
                 o.pngInterlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (colorType == PNG_COLOR_TYPE_PALETTE) {
        std::vector<png_color> pc(pal.size());
        std::vector<png_byte> trns;
        for (int i = 0; i < pal.size(); ++i) pc[i] = png_color{png_byte(qRed(pal[i])), png_byte(qGreen(pal[i])), png_byte(qBlue(pal[i]))};
        int lastTrans = -1;
        for (int i = 0; i < pal.size(); ++i) if (qAlpha(pal[i]) < 255) lastTrans = i;
        for (int i = 0; i <= lastTrans; ++i) trns.push_back(png_byte(qAlpha(pal[i])));
        png_set_PLTE(p, info, pc.data(), int(pc.size()));
        if (!trns.empty()) png_set_tRNS(p, info, trns.data(), int(trns.size()), nullptr);
    }
    if (o.dpi > 0) { const png_uint_32 ppm = png_uint_32(qRound(o.dpi / 0.0254)); png_set_pHYs(p, info, ppm, ppm, PNG_RESOLUTION_METER); }
    png_write_info(p, info);
    const int dep = depth, ct = colorType;   // plain copies for the pixel loops
    const int bps = dep == 16 ? 2 : 1;
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(pix.constScanLine(y));
        auto &r = rows[y];
        int nbytes = 0;
        switch (ct) {
        case PNG_COLOR_TYPE_RGB_ALPHA: nbytes = w * 4 * bps; break;
        case PNG_COLOR_TYPE_RGB: nbytes = w * 3 * bps; break;
        case PNG_COLOR_TYPE_GRAY: nbytes = w * bps; break;
        case PNG_COLOR_TYPE_GRAY_ALPHA: nbytes = w * 2 * bps; break;
        default: break;
        }
        if (nbytes) r.resize(size_t(nbytes));
        size_t ri = 0;
        auto put = [&](int v8) { if (bps == 2) { r[ri++] = png_byte(v8); r[ri++] = png_byte(v8); } else r[ri++] = png_byte(v8); };
        auto put16 = [&](int v16) { r[ri++] = png_byte(v16 >> 8); r[ri++] = png_byte(v16 & 0xFF); };
        switch (ct) {
        case PNG_COLOR_TYPE_RGB_ALPHA:
            for (int x = 0; x < w; ++x) { put(qRed(s[x])); put(qGreen(s[x])); put(qBlue(s[x])); put(qAlpha(s[x])); }
            break;
        case PNG_COLOR_TYPE_RGB:
            for (int x = 0; x < w; ++x) { put(qRed(s[x])); put(qGreen(s[x])); put(qBlue(s[x])); }
            break;
        case PNG_COLOR_TYPE_GRAY: case PNG_COLOR_TYPE_GRAY_ALPHA:
            for (int x = 0; x < w; ++x) {
                const double g = 0.299 * qRed(s[x]) + 0.587 * qGreen(s[x]) + 0.114 * qBlue(s[x]);
                if (bps == 2) put16(qBound(0, int(std::lround(g * 257.0)), 65535)); else r[ri++] = png_byte(qBound(0, int(std::lround(g)), 255));
                if (ct == PNG_COLOR_TYPE_GRAY_ALPHA) put(qAlpha(s[x]));
            }
            break;
        default: {   // palette, packed MSB first
            r.assign((size_t(w) * dep + 7) / 8, 0);
            const uchar *ix = idx.constData() + qsizetype(y) * w;
            for (int x = 0; x < w; ++x) {
                const int bit = x * dep;
                r[bit / 8] |= png_byte(ix[x] << (8 - dep - bit % 8));
            }
        }
        }
        rowPtrs[y] = r.data();
    }
    png_write_image(p, rowPtrs.data());
    png_write_end(p, info);
    png_destroy_write_struct(&p, &info);
    return true;
}

// ---- chunk helpers
QByteArray pngChunk(const QByteArray &png, const char type[4]) {
    qsizetype pos = 8;
    while (pos + 12 <= png.size()) {
        const uchar *b = reinterpret_cast<const uchar *>(png.constData() + pos);
        const quint32 len = quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | b[3];
        if (pos + 12 + qint64(len) > png.size()) break;
        if (memcmp(b + 4, type, 4) == 0) return png.mid(pos + 8, len);
        if (memcmp(b + 4, "IEND", 4) == 0) break;
        pos += 12 + len;
    }
    return {};
}

QByteArray pngWithChunk(const QByteArray &png, const char type[4], const QByteArray &payload) {
    qsizetype pos = 8, iend = -1;
    while (pos + 12 <= png.size()) {
        const uchar *b = reinterpret_cast<const uchar *>(png.constData() + pos);
        const quint32 len = quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | b[3];
        if (memcmp(b + 4, "IEND", 4) == 0) { iend = pos; break; }
        pos += 12 + len;
    }
    if (iend < 0) return png;
    QByteArray c;
    const quint32 n = quint32(payload.size());
    c.append(char(n >> 24)); c.append(char(n >> 16)); c.append(char(n >> 8)); c.append(char(n));
    c.append(type, 4);
    c.append(payload);
    uLong crc = crc32(0, reinterpret_cast<const Bytef *>(type), 4);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(payload.constData()), uInt(payload.size()));
    c.append(char(crc >> 24)); c.append(char(crc >> 16)); c.append(char(crc >> 8)); c.append(char(crc));
    QByteArray r = png;
    r.insert(iend, c);
    return r;
}

bool isFireworksPng(const QByteArray &d) {
    return !pngChunk(d, "mkBF").isNull() || !pngChunk(d, "mkBT").isNull() || !pngChunk(d, "mkBS").isNull() || !pngChunk(d, "mkTS").isNull();
}

} // namespace codec

// ---------------------------------------------------------------- Tiny compression
#ifdef LW_HAVE_IMAGEQUANT
#include <libimagequant.h>
#endif
#ifdef LW_HAVE_ZOPFLI
#include <zopfli/zopflipng_lib.h>
#endif

namespace codec {

bool tinyAvailable() {
#ifdef LW_HAVE_IMAGEQUANT
    return true;
#else
    return false;
#endif
}

QByteArray optimizePngLossless(const QByteArray &png, int speed) {
#ifdef LW_HAVE_ZOPFLI
    if (speed >= 8 || png.size() > 24 * 1024 * 1024) return png;
    CZopfliPNGOptions zo;
    CZopfliPNGSetDefaults(&zo);
    zo.lossy_transparent = 0;
    zo.lossy_8bit = 0;
    zo.auto_filter_strategy = 1;
    zo.use_zopfli = 1;
    // fewer iterations at higher speeds (zopflipng defaults: 15 / 5)
    zo.num_iterations = speed <= 2 ? 15 : speed <= 5 ? 8 : 3;
    zo.num_iterations_large = speed <= 2 ? 5 : speed <= 5 ? 3 : 1;
    unsigned char *res = nullptr;
    size_t resSize = 0;
    const int rc = CZopfliPNGOptimize(reinterpret_cast<const unsigned char *>(png.constData()), size_t(png.size()), &zo, 0, &res, &resSize);
    QByteArray out = png;
    if (rc == 0 && res && resSize > 0 && qsizetype(resSize) < png.size()) out = QByteArray(reinterpret_cast<const char *>(res), qsizetype(resSize));
    free(res);
    return out;
#else
    Q_UNUSED(speed);
    return png;
#endif
}

namespace {
double psnrRgba(const QImage &a, const QImage &b) {
    double se = 0;
    const qint64 n = qint64(a.width()) * a.height() * 4;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(a.constScanLine(y)), *t = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            // compare premultiplied: the colour of a fully transparent pixel is irrelevant
            const QRgb ps = qPremultiply(s[x]), pt = qPremultiply(t[x]);
            const int dr = qRed(ps) - qRed(pt), dg = qGreen(ps) - qGreen(pt), db = qBlue(ps) - qBlue(pt), da = qAlpha(ps) - qAlpha(pt);
            se += dr * dr + dg * dg + db * db + da * da;
        }
    }
    if (se <= 0 || n == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / double(n)));
}
}

bool tinyQuantizeFrames(QVector<QImage> &frames, const ExportOptions &o, QString *err) {
#ifdef LW_HAVE_IMAGEQUANT
    if (frames.isEmpty()) return true;
    liq_attr *attr = liq_attr_create();
    liq_set_quality(attr, qBound(0, o.tinyQualityMin, 100), qBound(0, qMax(o.tinyQualityMin, o.tinyQualityMax), 100));
    liq_set_speed(attr, qBound(1, o.tinySpeed, 10));
    liq_histogram *hist = liq_histogram_create(attr);
    std::vector<QByteArray> rgba(frames.size());
    std::vector<liq_image *> imgs(frames.size(), nullptr);
    for (int i = 0; i < frames.size(); ++i) {
        const QImage f = frames[i].convertToFormat(QImage::Format_RGBA8888);
        QByteArray buf(qsizetype(f.width()) * f.height() * 4, 0);
        for (int y = 0; y < f.height(); ++y) memcpy(buf.data() + qsizetype(y) * f.width() * 4, f.constScanLine(y), size_t(f.width()) * 4);
        rgba[i] = buf;
        imgs[i] = liq_image_create_rgba(attr, rgba[i].constData(), f.width(), f.height(), 0);
        liq_histogram_add_image(hist, attr, imgs[i]);
    }
    liq_result *res = nullptr;
    const liq_error e = liq_histogram_quantize(hist, attr, &res);
    bool ok = e == LIQ_OK;
    if (ok) {
        liq_set_dithering_level(res, qBound(0, o.tinyDither, 100) / 100.0f);
        for (int i = 0; i < frames.size(); ++i) {
            const int w = frames[i].width(), h = frames[i].height();
            std::vector<unsigned char> idx(size_t(w) * h);
            liq_write_remapped_image(res, imgs[i], idx.data(), idx.size());
            const liq_palette *pal = liq_get_palette(res);
            QImage q(w, h, QImage::Format_ARGB32);
            for (int y = 0; y < h; ++y) {
                QRgb *d = reinterpret_cast<QRgb *>(q.scanLine(y));
                for (int x = 0; x < w; ++x) { const liq_color c = pal->entries[idx[size_t(y) * w + x]]; d[x] = qRgba(c.r, c.g, c.b, c.a); }
            }
            frames[i] = q;
        }
    } else setErr(err, e == LIQ_QUALITY_TOO_LOW ? QStringLiteral("Tiny compression: quality range not reachable") : QStringLiteral("Tiny compression: quantisation failed"));
    if (res) liq_result_destroy(res);
    for (auto *im : imgs) liq_image_destroy(im);
    liq_histogram_destroy(hist);
    liq_attr_destroy(attr);
    return ok;
#else
    Q_UNUSED(frames); Q_UNUSED(o);
    setErr(err, QStringLiteral("Tiny compression needs libimagequant (not in this build)"));
    return false;
#endif
}

bool writeTinyPng(QByteArray &out, const QImage &src, const ExportOptions &o, QString *err, double *psnr) {
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
#ifdef LW_HAVE_IMAGEQUANT
    ExportOptions lossless = o;
    lossless.tinyPng = false; lossless.pngType = 0; lossless.png16 = false; lossless.pngInterlace = false; lossless.pngCompression = 9;
    const int w = img.width(), h = img.height();
    const QImage rgbaImg = img.convertToFormat(QImage::Format_RGBA8888);
    QByteArray buf(qsizetype(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) memcpy(buf.data() + qsizetype(y) * w * 4, rgbaImg.constScanLine(y), size_t(w) * 4);
    liq_attr *attr = liq_attr_create();
    liq_set_quality(attr, qBound(0, o.tinyQualityMin, 100), qBound(0, qMax(o.tinyQualityMin, o.tinyQualityMax), 100));
    liq_set_speed(attr, qBound(1, o.tinySpeed, 10));
    liq_image *li = liq_image_create_rgba(attr, buf.constData(), w, h, 0);
    liq_result *res = nullptr;
    const liq_error e = liq_image_quantize(li, attr, &res);
    if (e != LIQ_OK) {
        // like pngquant --skip-if-larger / quality failure: keep a lossless PNG
        liq_image_destroy(li); liq_attr_destroy(attr);
        if (!writePng(out, img, lossless, err)) return false;
        out = optimizePngLossless(out, o.tinySpeed);
        if (psnr) *psnr = 99.0;
        return true;
    }
    liq_set_dithering_level(res, qBound(0, o.tinyDither, 100) / 100.0f);
    std::vector<unsigned char> idx(size_t(w) * h);
    liq_write_remapped_image(res, li, idx.data(), idx.size());
    const liq_palette *lp = liq_get_palette(res);
    // translucent entries first keeps tRNS short
    std::vector<int> order(lp->count);
    for (unsigned i = 0; i < lp->count; ++i) order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return (lp->entries[a].a < 255) > (lp->entries[b].a < 255); });
    std::vector<int> remap(lp->count);
    QVector<QRgb> pal;
    for (int i = 0; i < int(order.size()); ++i) { remap[order[i]] = i; const liq_color c = lp->entries[order[i]]; pal << qRgba(c.r, c.g, c.b, c.a); }
    for (auto &v : idx) v = uchar(remap[v]);
    liq_result_destroy(res); liq_image_destroy(li); liq_attr_destroy(attr);
    // write the palette PNG with libpng (max compression, adaptive filters)
    const volatile int depth = pal.size() <= 2 ? 1 : pal.size() <= 4 ? 2 : pal.size() <= 16 ? 4 : 8;   // live across setjmp
    out.clear();
    QString perr;
    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, &perr, pngErrorCb, pngWarnCb);
    if (!p) { setErr(err, QStringLiteral("PNG: out of memory")); return false; }
    png_infop info = png_create_info_struct(p);
    if (!info) { png_destroy_write_struct(&p, nullptr); setErr(err, QStringLiteral("PNG: out of memory")); return false; }
    std::vector<std::vector<png_byte>> rows(h);
    std::vector<png_bytep> rowPtrs(h);
    if (setjmp(png_jmpbuf(p))) { png_destroy_write_struct(&p, &info); setErr(err, QStringLiteral("PNG: %1").arg(perr)); return false; }
    png_set_write_fn(p, &out, pngWriteCb, pngFlushCb);
    png_set_compression_level(p, 9);
    png_set_filter(p, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);   // palette images compress best unfiltered
    png_set_IHDR(p, info, png_uint_32(w), png_uint_32(h), depth, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    std::vector<png_color> pc(pal.size());
    std::vector<png_byte> trns;
    for (int i = 0; i < pal.size(); ++i) {
        pc[i] = png_color{png_byte(qRed(pal[i])), png_byte(qGreen(pal[i])), png_byte(qBlue(pal[i]))};
        if (qAlpha(pal[i]) < 255) trns.push_back(png_byte(qAlpha(pal[i])));
    }
    png_set_PLTE(p, info, pc.data(), int(pc.size()));
    if (!trns.empty()) png_set_tRNS(p, info, trns.data(), int(trns.size()), nullptr);
    if (o.dpi > 0) { const png_uint_32 ppm = png_uint_32(qRound(o.dpi / 0.0254)); png_set_pHYs(p, info, ppm, ppm, PNG_RESOLUTION_METER); }
    png_write_info(p, info);
    for (int y = 0; y < h; ++y) {
        auto &r = rows[y];
        const int dep = depth;
        r.assign((size_t(w) * dep + 7) / 8, 0);
        const unsigned char *ix = idx.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x) { const int bit = x * dep; r[bit / 8] |= png_byte(ix[x] << (8 - dep - bit % 8)); }
        rowPtrs[y] = r.data();
    }
    png_write_image(p, rowPtrs.data());
    png_write_end(p, info);
    png_destroy_write_struct(&p, &info);
    out = optimizePngLossless(out, o.tinySpeed);
    {   // --skip-if-larger: a lossless PNG 32 that is already smaller wins (smooth gradients, flat art)
        QByteArray ll;
        if (writePng(ll, img, lossless, nullptr) && ll.size() <= out.size()) {
            ll = optimizePngLossless(ll, o.tinySpeed);
            if (ll.size() <= out.size()) { out = ll; if (psnr) *psnr = 99.0; return true; }
        }
    }
    if (psnr) {
        QImage q(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(q.scanLine(y));
            for (int x = 0; x < w; ++x) d[x] = pal[idx[size_t(y) * w + x]];
        }
        *psnr = psnrRgba(img, q);
    }
    return true;
#else
    Q_UNUSED(psnr);
    setErr(err, QStringLiteral("Tiny compression needs libimagequant (not in this build)"));
    return false;
#endif
}

} // namespace codec
