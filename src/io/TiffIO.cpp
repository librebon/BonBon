#include "Brand.h"
// TIFF import/export through libtiff, using in-memory client I/O.
//
// Import: 8/16/32-bit integer and 32-bit float greyscale (min-is-black / min-is-white), RGB, CMYK
// and palette images, in strips, either planar configuration, with associated or unassociated alpha. Anything else
// libtiff can decode (tiles, 1/2/4-bit, YCbCr, CMYK, LAB, float, ...) goes through libtiff's
// RGBA interface. Every page of a multi-page file becomes one frame (reduced-resolution
// thumbnails are skipped).
// Export: RGB or RGBA (unassociated alpha), 8 or 16 bits per channel, None/LZW/Deflate/PackBits,
// one page per frame.
#include "Codecs.h"
#include <tiffio.h>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <vector>

namespace {

struct MemIO {
    QByteArray *wbuf = nullptr;      // write mode
    const QByteArray *rbuf = nullptr; // read mode
    toff_t pos = 0;
    QString lastError;
    const QByteArray &data() const { return wbuf ? *wbuf : *rbuf; }
};

tsize_t memRead(thandle_t h, tdata_t p, tsize_t n) {
    auto *m = static_cast<MemIO *>(h);
    const QByteArray &d = m->data();
    if (m->pos >= toff_t(d.size())) return 0;
    tsize_t avail = tsize_t(d.size() - qint64(m->pos));
    if (n > avail) n = avail;
    std::memcpy(p, d.constData() + m->pos, size_t(n));
    m->pos += toff_t(n);
    return n;
}
tsize_t memWrite(thandle_t h, tdata_t p, tsize_t n) {
    auto *m = static_cast<MemIO *>(h);
    if (!m->wbuf) return -1;
    const qint64 end = qint64(m->pos) + qint64(n);
    if (end > m->wbuf->size()) m->wbuf->resize(end);
    std::memcpy(m->wbuf->data() + m->pos, p, size_t(n));
    m->pos = toff_t(end);
    return n;
}
toff_t memSeek(thandle_t h, toff_t off, int whence) {
    auto *m = static_cast<MemIO *>(h);
    qint64 base = whence == SEEK_CUR ? qint64(m->pos) : whence == SEEK_END ? m->data().size() : 0;
    qint64 np = base + qint64(off);
    if (np < 0) return toff_t(-1);
    if (m->wbuf && np > m->wbuf->size()) m->wbuf->resize(np);
    m->pos = toff_t(np);
    return m->pos;
}
int memClose(thandle_t) { return 0; }
toff_t memSize(thandle_t h) { return toff_t(static_cast<MemIO *>(h)->data().size()); }
int memMap(thandle_t, tdata_t *, toff_t *) { return 0; }
void memUnmap(thandle_t, tdata_t, toff_t) {}

int onError(TIFF *, void *user, const char *module, const char *fmt, va_list ap) {
    char msg[512];
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    auto *m = static_cast<MemIO *>(user);
    if (m && m->lastError.isEmpty()) m->lastError = QStringLiteral("%1: %2").arg(QString::fromLocal8Bit(module ? module : "libtiff"), QString::fromLocal8Bit(msg));
    return 1;
}
int onWarning(TIFF *, void *, const char *, const char *, va_list) { return 1; } // quiet

TIFF *openMem(MemIO &m, const char *mode) {
    TIFFOpenOptions *opts = TIFFOpenOptionsAlloc();
    TIFFOpenOptionsSetErrorHandlerExtR(opts, onError, &m);
    TIFFOpenOptionsSetWarningHandlerExtR(opts, onWarning, &m);
    TIFFOpenOptionsSetMaxSingleMemAlloc(opts, tmsize_t(1) << 31);
    TIFF *t = TIFFClientOpenExt("memory", mode, &m, memRead, memWrite, memSeek, memClose, memSize, memMap, memUnmap, opts);
    TIFFOpenOptionsFree(opts);
    return t;
}

constexpr qint64 kMaxPixels = qint64(1) << 28;


QImage applyOrientation(const QImage &img, int orient) {
    switch (orient) {
    case ORIENTATION_TOPRIGHT: return img.mirrored(true, false);
    case ORIENTATION_BOTRIGHT: return img.mirrored(true, true);
    case ORIENTATION_BOTLEFT: return img.mirrored(false, true);
    case ORIENTATION_LEFTTOP: return img.transformed(QTransform(0, 1, 1, 0, 0, 0));      // transpose
    case ORIENTATION_RIGHTTOP: return img.transformed(QTransform().rotate(90));
    case ORIENTATION_RIGHTBOT: return img.transformed(QTransform(0, -1, -1, 0, 0, 0));   // transverse
    case ORIENTATION_LEFTBOT: return img.transformed(QTransform().rotate(270));
    default: return img;
    }
}

// libtiff's generic RGBA path: handles everything libtiff knows, output is associated alpha.
bool readPageRgba(TIFF *tif, uint32_t w, uint32_t h, QImage &img, MemIO &m, QString *err) {
    char emsg[1024] = {0};
    if (!TIFFRGBAImageOK(tif, emsg)) { codec::setErr(err, QStringLiteral("Unsupported TIFF: %1").arg(QString::fromLocal8Bit(emsg))); return false; }
    std::vector<uint32_t> raster(size_t(w) * h);
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster.data(), ORIENTATION_TOPLEFT, 0)) {
        codec::setErr(err, m.lastError.isEmpty() ? QStringLiteral("Could not decode TIFF") : m.lastError);
        return false;
    }
    img = QImage(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
    for (uint32_t y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(img.scanLine(int(y)));
        const uint32_t *s = raster.data() + size_t(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t p = s[x];
            int a = TIFFGetA(p);
            d[x] = qRgba(qMin<int>(TIFFGetR(p), a), qMin<int>(TIFFGetG(p), a), qMin<int>(TIFFGetB(p), a), a);
        }
    }
    img = img.convertToFormat(QImage::Format_ARGB32);
    return true;
}

bool readPage(TIFF *tif, QImage &img, MemIO &m, QString *err) {
    uint32_t w = 0, h = 0;
    uint16_t bps = 1, spp = 1, photo = PHOTOMETRIC_MINISBLACK, planar = PLANARCONFIG_CONTIG, sfmt = SAMPLEFORMAT_UINT, orient = ORIENTATION_TOPLEFT;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (!w || !h || qint64(w) * h > kMaxPixels) { codec::setErr(err, QStringLiteral("TIFF has invalid or too large dimensions (%1x%2)").arg(w).arg(h)); return false; }
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo)) photo = spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sfmt);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orient);
    uint16_t nExtra = 0; uint16_t *extra = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &nExtra, &extra);

    uint16_t inkset = INKSET_CMYK;
    TIFFGetFieldDefaulted(tif, TIFFTAG_INKSET, &inkset);
    const bool cmyk = photo == PHOTOMETRIC_SEPARATED && inkset == INKSET_CMYK;
    const int nc = photo == PHOTOMETRIC_RGB ? 3 : cmyk ? 4 : 1;
    const bool intOk = sfmt == SAMPLEFORMAT_UINT && (bps == 8 || bps == 16 || bps == 32);
    const bool fltOk = sfmt == SAMPLEFORMAT_IEEEFP && bps == 32 && photo != PHOTOMETRIC_PALETTE;
    const bool direct = !TIFFIsTiled(tif) && (intOk || fltOk) &&
                        (photo == PHOTOMETRIC_MINISBLACK || photo == PHOTOMETRIC_MINISWHITE || photo == PHOTOMETRIC_RGB ||
                         cmyk || (photo == PHOTOMETRIC_PALETTE && bps <= 16)) && spp >= nc && spp <= nc + 8;
    if (!direct) return readPageRgba(tif, w, h, img, m, err);

    const bool hasAlpha = spp > nc;
    // An alpha channel is associated (premultiplied) only when tagged so; unassociated and
    // unspecified extra samples are both treated as straight alpha, as most readers do.
    const bool assoc = hasAlpha && nExtra > 0 && extra && extra[0] == EXTRASAMPLE_ASSOCALPHA;
    const int used = nc + (hasAlpha ? 1 : 0);
    const double maxv = fltOk ? 1.0 : double((uint64_t(1) << bps) - 1);

    uint16_t *cmr = nullptr, *cmg = nullptr, *cmb = nullptr;
    bool cmap8 = false;
    if (photo == PHOTOMETRIC_PALETTE) {
        if (!TIFFGetField(tif, TIFFTAG_COLORMAP, &cmr, &cmg, &cmb)) { codec::setErr(err, QStringLiteral("Palette TIFF without a colour map")); return false; }
        cmap8 = true; // old writers store 8-bit values in the 16-bit colour map
        for (uint32_t i = 0; i < (1u << bps) && cmap8; ++i) if (cmr[i] > 255 || cmg[i] > 255 || cmb[i] > 255) cmap8 = false;
    }

    // Samples normalised to 0..1 (palette indices kept as raw values).
    std::vector<float> px(size_t(w) * h * used);
    const tmsize_t lineSize = TIFFScanlineSize(tif);
    std::vector<uint8_t> line(size_t(lineSize > 0 ? lineSize : 1));
    const bool isPal = photo == PHOTOMETRIC_PALETTE;
    auto sampleAt = [&](const uint8_t *buf, size_t idx) -> float {
        double v;
        if (fltOk) v = reinterpret_cast<const float *>(buf)[idx];
        else if (bps == 8) v = buf[idx];
        else if (bps == 16) v = reinterpret_cast<const uint16_t *>(buf)[idx];
        else v = reinterpret_cast<const uint32_t *>(buf)[idx];
        return float(isPal ? v : v / maxv);
    };
    auto readErr = [&] { codec::setErr(err, m.lastError.isEmpty() ? QStringLiteral("TIFF read error") : m.lastError); return false; };
    if (planar == PLANARCONFIG_CONTIG) {
        for (uint32_t y = 0; y < h; ++y) {
            if (TIFFReadScanline(tif, line.data(), y, 0) < 0) return readErr();
            float *d = px.data() + size_t(y) * w * used;
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < used; ++c) d[x * used + c] = sampleAt(line.data(), size_t(x) * spp + c);
        }
    } else {
        for (int c = 0; c < used; ++c)
            for (uint32_t y = 0; y < h; ++y) {
                if (TIFFReadScanline(tif, line.data(), y, uint16_t(c)) < 0) return readErr();
                float *d = px.data() + size_t(y) * w * used;
                for (uint32_t x = 0; x < w; ++x) d[x * used + c] = sampleAt(line.data(), x);
            }
    }

    img = QImage(int(w), int(h), QImage::Format_ARGB32);
    if (img.isNull()) { codec::setErr(err, QStringLiteral("Out of memory decoding TIFF")); return false; }
    auto q8 = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    for (uint32_t y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(img.scanLine(int(y)));
        const float *s = px.data() + size_t(y) * w * used;
        for (uint32_t x = 0; x < w; ++x, s += used) {
            double a = hasAlpha ? std::clamp<double>(s[nc], 0.0, 1.0) : 1.0;
            double c[3];
            if (isPal) {
                uint32_t i = uint32_t(s[0]) & ((1u << bps) - 1);
                const double div = cmap8 ? 255.0 : 65535.0;
                c[0] = cmr[i] / div; c[1] = cmg[i] / div; c[2] = cmb[i] / div;
            } else if (nc == 3) { c[0] = s[0]; c[1] = s[1]; c[2] = s[2]; }
            else if (cmyk) { const double k = 1.0 - s[3]; c[0] = (1.0 - s[0]) * k; c[1] = (1.0 - s[1]) * k; c[2] = (1.0 - s[2]) * k; }
            else { double g = photo == PHOTOMETRIC_MINISWHITE ? 1.0 - s[0] : s[0]; c[0] = c[1] = c[2] = g; }
            if (assoc && a > 0 && a < 1) for (double &k : c) k = std::min(1.0, k / a);
            d[x] = qRgba(q8(c[0]), q8(c[1]), q8(c[2]), q8(a));
        }
    }
    img = applyOrientation(img, orient);
    return true;
}

} // namespace

namespace codec {

bool readTiff(const QByteArray &d, ImageSeq &out, QString *err) {
    MemIO m;
    m.rbuf = &d;
    TIFF *tif = openMem(m, "rm");
    if (!tif) { setErr(err, m.lastError.isEmpty() ? QStringLiteral("Not a valid TIFF file") : m.lastError); return false; }
    QVector<QImage> pages;
    QString firstErr;
    int dirs = 0;
    do {
        if (++dirs > 4096) break;
        uint32_t sub = 0;
        TIFFGetField(tif, TIFFTAG_SUBFILETYPE, &sub);
        if ((sub & FILETYPE_REDUCEDIMAGE) && !pages.isEmpty()) continue; // thumbnail / preview
        QImage img;
        QString e;
        m.lastError.clear();
        if (readPage(tif, img, m, &e)) pages << img;
        else if (firstErr.isEmpty()) firstErr = e;
    } while (TIFFReadDirectory(tif));
    TIFFClose(tif);
    if (pages.isEmpty()) { setErr(err, firstErr.isEmpty() ? QStringLiteral("TIFF has no readable pages") : firstErr); return false; }
    // Frames share one canvas: pages of other sizes are placed at the top-left of the largest.
    int W = 0, H = 0;
    for (const QImage &p : pages) { W = qMax(W, p.width()); H = qMax(H, p.height()); }
    out.frames.clear();
    for (QImage &p : pages) {
        if (p.size() != QSize(W, H)) {
            QImage c(W, H, QImage::Format_ARGB32);
            c.fill(Qt::transparent);
            for (int y = 0; y < p.height(); ++y) std::memcpy(c.scanLine(y), p.constScanLine(y), size_t(p.width()) * 4);
            p = c;
        }
        out.frames.append({p, 100});
    }
    out.width = W;
    out.height = H;
    out.loops = 0;
    return true;
}

bool writeTiff(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err) {
    static const uint16_t kComp[] = {COMPRESSION_NONE, COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE, COMPRESSION_PACKBITS};
    const uint16_t comp = kComp[qBound(0, o.tiffCompression, 3)];
    if (!TIFFIsCODECConfigured(comp)) { setErr(err, QStringLiteral("This libtiff build lacks the requested compression")); return false; }
    out.clear();
    MemIO m;
    m.wbuf = &out;
    TIFF *tif = openMem(m, "w");
    if (!tif) { setErr(err, m.lastError.isEmpty() ? QStringLiteral("Could not create TIFF") : m.lastError); return false; }
    const int bps = o.tiff16 ? 16 : 8;
    const int spp = o.alpha ? 4 : 3;
    const int n = seq.frames.size();
    bool ok = true;
    for (int i = 0; i < n && ok; ++i) {
        QImage img = o.alpha ? seq.frames[i].image.convertToFormat(QImage::Format_ARGB32) : flatten(seq.frames[i].image, o.matte);
        const uint32_t w = uint32_t(img.width()), h = uint32_t(img.height());
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t(bps));
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t(spp));
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, comp);
        if (comp == COMPRESSION_LZW || comp == COMPRESSION_ADOBE_DEFLATE) TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
        if (o.alpha) { uint16_t ex[1] = {EXTRASAMPLE_UNASSALPHA}; TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, ex); }
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
        TIFFSetField(tif, TIFFTAG_XRESOLUTION, 72.0f);
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, 72.0f);
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        {
            const QByteArray sw = QByteArray(LW_PRODUCT_NAME) + " " LW_VERSION;
            TIFFSetField(tif, TIFFTAG_SOFTWARE, sw.constData());
        }
        if (n > 1) {
            TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_PAGE);
            TIFFSetField(tif, TIFFTAG_PAGENUMBER, uint16_t(i), uint16_t(n));
        }
        std::vector<uint8_t> line(size_t(w) * spp * (bps / 8));
        for (uint32_t y = 0; y < h && ok; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(int(y)));
            for (uint32_t x = 0; x < w; ++x) {
                const int v[4] = {qRed(s[x]), qGreen(s[x]), qBlue(s[x]), qAlpha(s[x])};
                for (int c = 0; c < spp; ++c) {
                    if (bps == 8) line[size_t(x) * spp + c] = uint8_t(v[c]);
                    else reinterpret_cast<uint16_t *>(line.data())[size_t(x) * spp + c] = uint16_t(v[c] * 257);
                }
            }
            if (TIFFWriteScanline(tif, line.data(), y, 0) < 0) ok = false;
        }
        if (ok && !TIFFWriteDirectory(tif)) ok = false;
    }
    TIFFClose(tif);
    if (!ok) { setErr(err, m.lastError.isEmpty() ? QStringLiteral("TIFF write error") : m.lastError); out.clear(); }
    return ok;
}

} // namespace codec
