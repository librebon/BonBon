#include "Brand.h"
#include "core/PixelScale.h"
#include "ImageIO.h"
#include "Codecs.h"
#include "DocIO.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QSettings>
#include <cmath>
#include <cstring>

static bool g_chStripDefault = true;
bool cheader_stripDefault() { return g_chStripDefault; }
void cheader_setStripDefault(bool on) { g_chStripDefault = on; }

bool ExportOptions::operator==(const ExportOptions &o) const {
    return area == o.area && format == o.format && quality == o.quality && lossless == o.lossless && webpEffort == o.webpEffort && webpAlphaQuality == o.webpAlphaQuality && webpSharpYuv == o.webpSharpYuv && webpExact == o.webpExact && avifSpeed == o.avifSpeed && avifChroma == o.avifChroma && avifDepth == o.avifDepth && avifAlphaQuality == o.avifAlphaQuality && jxlEffort == o.jxlEffort && heifPreset == o.heifPreset && heifChroma == o.heifChroma && colors == o.colors &&
           palette == o.palette && dither == o.dither && ditherAmount == o.ditherAmount && loss == o.loss && animated == o.animated && netpbmAscii == o.netpbmAscii &&
           tiffCompression == o.tiffCompression && tiff16 == o.tiff16 && alpha == o.alpha && tgaRle == o.tgaRle &&
           tgaTopLeft == o.tgaTopLeft &&
           pngType == o.pngType && png16 == o.png16 && pngInterlace == o.pngInterlace && pngCompression == o.pngCompression &&
           pngTransparency == o.pngTransparency && tinyPng == o.tinyPng && tinyQualityMin == o.tinyQualityMin &&
           tinyQualityMax == o.tinyQualityMax && tinySpeed == o.tinySpeed && customPalette == o.customPalette &&
           pngEditable == o.pngEditable && bmpBits == o.bmpBits && bmpRle == o.bmpRle && icoSizes == o.icoSizes &&
           icnsSizes == o.icnsSizes && curHotX == o.curHotX && curHotY == o.curHotY && ddsFormat == o.ddsFormat &&
           mipmaps == o.mipmaps && ktxZstd == o.ktxZstd && ktxZstdLevel == o.ktxZstdLevel && tinyDither == o.tinyDither && exrCompression == o.exrCompression && exrHalf == o.exrHalf &&
           pcx8 == o.pcx8 && jpegEncoder == o.jpegEncoder && jpegProgressive == o.jpegProgressive &&
           jpegOptimize == o.jpegOptimize && jpegSubsampling == o.jpegSubsampling && jpegSmoothing == o.jpegSmoothing &&
           jpegSharpenEdges == o.jpegSharpenEdges && jpegStripMeta == o.jpegStripMeta && jpegKeepIcc == o.jpegKeepIcc &&
           chPixel == o.chPixel && chElement == o.chElement && chHex == o.chHex && chPerLine == o.chPerLine && chConst == o.chConst &&
           chProgmem == o.chProgmem && chAligned == o.chAligned && chSection == o.chSection && chSectionName == o.chSectionName &&
           chStdint == o.chStdint && chDefines == o.chDefines && chFrameLayout == o.chFrameLayout && chDelays == o.chDelays &&
           chRle == o.chRle && chStyle == o.chStyle && chRawBin == o.chRawBin && chLsbFirst == o.chLsbFirst && chInvert == o.chInvert &&
           chName == o.chName && chGimpMacros == o.chGimpMacros && chGimpGlib == o.chGimpGlib && chGimpComment == o.chGimpComment &&
           chGimpCommentText == o.chGimpCommentText && chGimpOpacity == o.chGimpOpacity && chStrip == o.chStrip && chDither == o.chDither && matte == o.matte &&
           scalePct == o.scalePct && scaleResample == o.scaleResample;
}

namespace {
struct FmtInfo { Fmt f; const char *key; const char *name; const char *ext; bool anim; bool quality; bool lossless; const char *exts; const char *mime; };
const FmtInfo kFormats[] = {
    {Fmt::PNG, "png", "PNG", "png", false, false, false, "png", "image/png"},
    {Fmt::APNG, "apng", "APNG (animated PNG)", "png", true, false, false, "apng png", "image/apng"},
    {Fmt::JPEG, "jpeg", "JPEG", "jpg", false, true, false, "jpg jpeg jpe jfif", "image/jpeg"},
    {Fmt::WEBP, "webp", "WebP", "webp", true, true, true, "webp", "image/webp"},
    {Fmt::GIF, "gif", "GIF", "gif", true, false, false, "gif", "image/gif"},
    {Fmt::AVIF, "avif", "AVIF", "avif", true, true, true, "avif avifs", "image/avif"},
    {Fmt::JXL, "jxl", "JPEG XL", "jxl", true, true, true, "jxl", "image/jxl"},
    {Fmt::HEIC, "heic", "HEIC / HEIF", "heic", false, true, true, "heic heif heics heifs hif", "image/heic image/heif image/heic-sequence image/heif-sequence"},
    {Fmt::BMP, "bmp", "BMP", "bmp", false, false, false, "bmp dib", "image/bmp image/x-bmp image/x-ms-bmp"},
    {Fmt::TIFF, "tiff", "TIFF", "tif", true, false, false, "tif tiff", "image/tiff"},
    {Fmt::TGA, "tga", "TGA (Targa)", "tga", false, false, false, "tga icb vda vst tpic", "image/x-tga image/x-targa"},
    {Fmt::PSD, "psd", "Photoshop (PSD, layered)", "psd", false, false, false, "psd psb", "image/vnd.adobe.photoshop image/x-psd application/x-photoshop"},
    {Fmt::ORA, "ora", "OpenRaster (ORA, layered)", "ora", false, false, false, "ora", "image/openraster"},
    {Fmt::XCF, "xcf", "GIMP XCF (layered)", "xcf", false, false, false, "xcf", "image/x-xcf"},
    {Fmt::SVG, "svg", "SVG", "svg", false, false, false, "svg", "image/svg+xml"},
    {Fmt::SVGZ, "svgz", "SVGZ (compressed SVG)", "svgz", false, false, false, "svgz", "image/svg+xml-compressed"},
    {Fmt::PDF, "pdf", "PDF", "pdf", false, false, false, "pdf", "application/pdf"},
    {Fmt::RAW, "raw", "Camera RAW", "dng", false, false, false,
     "dng cr2 cr3 crw nef nrw arw srf sr2 orf rw2 raf pef ptx srw x3f erf mef mos mrw kdc dcr 3fr iiq rwl",
     "image/x-dcraw image/x-adobe-dng image/x-canon-cr2 image/x-canon-cr3 image/x-canon-crw image/x-nikon-nef image/x-nikon-nrw image/x-sony-arw image/x-sony-srf image/x-sony-sr2 image/x-olympus-orf image/x-panasonic-rw2 image/x-fuji-raf image/x-pentax-pef image/x-samsung-srw image/x-sigma-x3f image/x-kodak-dcr image/x-kodak-kdc image/x-minolta-mrw image/x-epson-erf image/x-hasselblad-3fr"},
    {Fmt::EXR, "exr", "OpenEXR (HDR)", "exr", false, false, false, "exr", "image/x-exr"},
    {Fmt::HDR, "hdr", "Radiance HDR (RGBE)", "hdr", false, false, false, "hdr pic rgbe", "image/vnd.radiance image/x-hdr"},
    {Fmt::PFM, "pfm", "PFM (portable float map)", "pfm", false, false, false, "pfm", "image/x-portable-floatmap"},
    {Fmt::DDS, "dds", "DDS (DirectDraw Surface)", "dds", false, false, false, "dds", "image/vnd.ms-dds image/x-dds"},
    {Fmt::KTX2, "ktx2", "KTX2 texture", "ktx2", false, false, false, "ktx2", "image/ktx2"},
    {Fmt::QOI, "qoi", "QOI", "qoi", false, false, false, "qoi", "image/qoi image/x-qoi"},
    {Fmt::ICO, "ico", "Windows icon (ICO)", "ico", false, false, false, "ico", "image/vnd.microsoft.icon image/x-icon"},
    {Fmt::CUR, "cur", "Windows cursor (CUR)", "cur", false, false, false, "cur", "image/x-win-bitmap image/x-cursor"},
    {Fmt::ICNS, "icns", "Apple icon (ICNS)", "icns", false, false, false, "icns", "image/x-icns"},
    {Fmt::PPM, "ppm", "PPM (Netpbm colour)", "ppm", false, false, false, "ppm pnm", "image/x-portable-pixmap image/x-portable-anymap"},
    {Fmt::PGM, "pgm", "PGM (Netpbm greyscale)", "pgm", false, false, false, "pgm", "image/x-portable-graymap"},
    {Fmt::PBM, "pbm", "PBM (Netpbm bitmap)", "pbm", false, false, false, "pbm", "image/x-portable-bitmap"},
    {Fmt::PAM, "pam", "PAM (Netpbm with alpha)", "pam", false, false, false, "pam", "image/x-portable-arbitrarymap"},
    {Fmt::PCX, "pcx", "PCX", "pcx", false, false, false, "pcx", "image/vnd.zbrush.pcx image/x-pcx"},
    {Fmt::XPM, "xpm", "XPM", "xpm", false, false, false, "xpm", "image/x-xpixmap"},
    {Fmt::XBM, "xbm", "XBM", "xbm", false, false, false, "xbm", "image/x-xbitmap"},
    {Fmt::CHeader, "cheader", "C Header (.h)", "h", true, false, false, "h", "text/x-chdr"},
};
const FmtInfo *info(Fmt f) {
    for (auto &i : kFormats) if (i.f == f) return &i;
    return nullptr;
}
QStringList splitExts(const char *s) { return QString::fromLatin1(s).split(QLatin1Char(' '), Qt::SkipEmptyParts); }

// ISO-BMFF: is `brand` the major or a compatible brand of the ftyp box?
bool ftypHas(const QByteArray &h, const char *brand) {
    if (h.size() < 16 || memcmp(h.constData() + 4, "ftyp", 4) != 0) return false;
    const quint32 len = (quint32(uchar(h[0])) << 24) | (quint32(uchar(h[1])) << 16) | (quint32(uchar(h[2])) << 8) | uchar(h[3]);
    const int end = int(qMin<quint32>(len, quint32(h.size())));
    if (memcmp(h.constData() + 8, brand, 4) == 0) return true;
    for (int p = 16; p + 4 <= end; p += 4) if (memcmp(h.constData() + p, brand, 4) == 0) return true;
    return false;
}
bool looksLikeSvgText(const QByteArray &h) {
    QByteArray s = h.left(4096);
    if (s.startsWith("\xEF\xBB\xBF")) s = s.mid(3);
    s = s.trimmed();
    if (!s.startsWith('<')) return false;
    return s.contains("<svg") || (s.contains("<!DOCTYPE svg"));
}
bool looksLikePcx(const QByteArray &h) {
    if (h.size() < 128 || uchar(h[0]) != 0x0A || uchar(h[2]) > 1) return false;
    const int v = uchar(h[1]);
    const int bpp = uchar(h[3]);
    return (v == 0 || v == 2 || v == 3 || v == 4 || v == 5) && (bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8) && uchar(h[65]) >= 1 && uchar(h[65]) <= 4;
}
}

namespace ImageIO {

QString formatName(Fmt f) { auto i = info(f); return i ? QString::fromLatin1(i->name) : QStringLiteral("Unknown"); }
QString formatKey(Fmt f) { auto i = info(f); return i ? QString::fromLatin1(i->key) : QString(); }
Fmt formatFromKey(const QString &k) { for (auto &i : kFormats) if (k == QLatin1String(i.key)) return i.f; return Fmt::PNG; }
QString extension(Fmt f) { auto i = info(f); return i ? QString::fromLatin1(i->ext) : QString(); }
QStringList extensions(Fmt f) { auto i = info(f); return i ? splitExts(i->exts) : QStringList(); }
bool canAnimate(Fmt f) { auto i = info(f); return i && i->anim; }
bool hasQuality(Fmt f) { auto i = info(f); return i && i->quality; }
bool hasLossless(Fmt f) { auto i = info(f); return i && i->lossless; }
bool hasAlpha(const ExportOptions &o) {
    switch (o.format) {
    case Fmt::JPEG: case Fmt::PPM: case Fmt::PGM: case Fmt::PBM: case Fmt::HDR: case Fmt::PFM: case Fmt::XBM: case Fmt::PDF: return false;
    case Fmt::PNG:
        if (o.tinyPng) return true;
        return o.pngType == 0 || (o.pngType == 2 && o.pngTransparency > 0) || (o.pngType == 3 && o.alpha);
    case Fmt::BMP: return o.bmpBits == 32;
    case Fmt::TIFF: case Fmt::TGA: case Fmt::PAM: return o.alpha;
    case Fmt::PCX: return !o.pcx8 && o.alpha;
    case Fmt::CHeader: return (o.chStyle == 0 && cheader::hasAlpha(o.chPixel)) || (o.chStyle == 2 && cheader::gimpBytesPerPixel(o.chPixel) == 4);
    default: return true;
    }
}

bool canRead(Fmt f) {
    switch (f) {
    case Fmt::PDF: case Fmt::CHeader: case Fmt::Unknown: return false;
#ifndef LW_HAVE_AVIF
    case Fmt::AVIF: return false;
#endif
#ifndef LW_HAVE_JXL
    case Fmt::JXL: return false;
#endif
#ifndef LW_HAVE_OPENEXR
    case Fmt::EXR: return false;
#endif
#ifndef LW_HAVE_RAW
    case Fmt::RAW: return false;
#endif
    case Fmt::HEIC: return codec::heifCanDecode();
    default: return true;
    }
}
bool canWrite(Fmt f) {
    switch (f) {
    case Fmt::RAW: case Fmt::Unknown: return false;
#ifndef LW_HAVE_AVIF
    case Fmt::AVIF: return false;
#endif
#ifndef LW_HAVE_JXL
    case Fmt::JXL: return false;
#endif
#ifndef LW_HAVE_OPENEXR
    case Fmt::EXR: return false;
#endif
    case Fmt::HEIC: return codec::heifCanEncode();
    default: return true;
    }
}
bool available(Fmt f) { return canRead(f) || canWrite(f); }
bool isLayered(Fmt f) { return f == Fmt::PSD || f == Fmt::ORA || f == Fmt::XCF || f == Fmt::SVG || f == Fmt::SVGZ || f == Fmt::PDF; }
bool isHdr(Fmt f) { return f == Fmt::EXR || f == Fmt::HDR || f == Fmt::PFM; }
const QVector<int> &iconSizes() { static const QVector<int> s{16, 24, 32, 48, 64, 128, 256, 512, 1024}; return s; }

QList<Fmt> exportFormats() {
    QList<Fmt> l;
    for (auto &i : kFormats) if (canWrite(i.f)) l << i.f;
    return l;
}
QList<Fmt> importFormats() {
    QList<Fmt> l;
    for (auto &i : kFormats) if (canRead(i.f)) l << i.f;
    return l;
}
QStringList mimeTypes() {
    QStringList l;
    for (auto &i : kFormats) if (canRead(i.f)) l << QString::fromLatin1(i.mime).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    l.removeDuplicates();
    return l;
}
QString mimeType(Fmt f) {
    auto i = info(f);
    if (!i || !i->mime || !i->mime[0]) return {};
    return QString::fromLatin1(i->mime).split(QLatin1Char(' '), Qt::SkipEmptyParts).value(0);
}

Fmt detect(const QByteArray &h) {
    auto at = [&](int off, const char *magic, int n) { return h.size() >= off + n && memcmp(h.constData() + off, magic, n) == 0; };
    if (at(0, "\x89PNG\r\n\x1a\n", 8)) return codec::isApng(h) ? Fmt::APNG : Fmt::PNG;
    if (at(0, "\xFF\xD8\xFF", 3)) return Fmt::JPEG;
    if (at(0, "GIF87a", 6) || at(0, "GIF89a", 6)) return Fmt::GIF;
    if (at(0, "RIFF", 4) && at(8, "WEBP", 4)) return Fmt::WEBP;
    if (at(4, "ftyp", 4)) {
        if (ftypHas(h, "crx ")) return Fmt::RAW;                     // Canon CR3
        if (ftypHas(h, "avif") || ftypHas(h, "avis")) return Fmt::AVIF;
        if (ftypHas(h, "heic") || ftypHas(h, "heix") || ftypHas(h, "hevc") || ftypHas(h, "hevx") || ftypHas(h, "heim") ||
            ftypHas(h, "heis") || ftypHas(h, "hevm") || ftypHas(h, "hevs") || ftypHas(h, "mif1") || ftypHas(h, "msf1"))
            return Fmt::HEIC;
    }
    if (at(0, "\xFF\x0A", 2) || at(0, "\x00\x00\x00\x0CJXL \x0D\x0A\x87\x0A", 12)) return Fmt::JXL;
    if (at(0, "8BPS", 4)) return Fmt::PSD;
    if (at(0, "gimp xcf ", 9)) return Fmt::XCF;
    if (at(0, "PK\x03\x04", 4) && at(30, "mimetypeimage/openraster", 24)) return Fmt::ORA;
    if (at(0, "%PDF-", 5)) return Fmt::PDF;
    if (at(0, "\x76\x2F\x31\x01", 4)) return Fmt::EXR;
    if (at(0, "#?RADIANCE", 10) || at(0, "#?RGBE", 6)) return Fmt::HDR;
    if (at(0, "DDS ", 4)) return Fmt::DDS;
    if (at(0, "\xABKTX 20\xBB\r\n\x1A\n", 12)) return Fmt::KTX2;
    if (at(0, "qoif", 4)) return Fmt::QOI;
    if (at(0, "icns", 4)) return Fmt::ICNS;
    if (at(0, "FUJIFILMCCD-RAW", 15) || at(0, "IIRO", 4) || at(0, "IIRS", 4) || at(0, "MMOR", 4) || at(0, "IIU\0", 4) ||
        at(0, "FOVb", 4) || (at(0, "II*\0", 4) && at(8, "CR", 2)) || at(0, "\0MRM", 4))
        return Fmt::RAW;
    if (at(0, "BM", 2)) return Fmt::BMP;
    if (at(0, "II*\0", 4) || at(0, "MM\0*", 4)) return Fmt::TIFF;
    if (h.size() >= 6 && at(0, "\0\0", 2) && (at(2, "\x01\0", 2) || at(2, "\x02\0", 2))) {
        const int n = uchar(h[4]) | (uchar(h[5]) << 8);
        // ICO: colour planes 0 / 1; CUR: that field holds the hotspot x, so check the image offset instead
        const quint32 off = h.size() >= 22 ? (uchar(h[18]) | uchar(h[19]) << 8 | uchar(h[20]) << 16 | (quint32(uchar(h[21])) << 24)) : 0;
        if (n > 0 && n < 256 && h.size() >= 6 + 16 && h[3] == 0 && off >= quint32(6 + 16 * n) && (h[2] == 2 || uchar(h[6 + 4]) <= 1))
            return h[2] == 1 ? Fmt::ICO : Fmt::CUR;
    }
    if (h.size() >= 2 && h[0] == 'P') {
        switch (h[1]) {
        case '1': case '4': return Fmt::PBM;
        case '2': case '5': return Fmt::PGM;
        case '3': case '6': return Fmt::PPM;
        case '7': return Fmt::PAM;
        case 'F': case 'f': return Fmt::PFM;
        }
    }
    if (at(0, "\x1F\x8B", 2)) return Fmt::SVGZ;       // gzip: the only gzip-wrapped format we read
    if (h.left(256).contains("/* XPM */") || at(0, "! XPM2", 6)) return Fmt::XPM;
    if (at(0, "#define ", 8) && h.left(512).contains("_width")) return Fmt::XBM;
    if (looksLikeSvgText(h)) return Fmt::SVG;
    if (looksLikePcx(h)) return Fmt::PCX;
    if (codec::looksLikeTga(h)) return Fmt::TGA; // no magic number: header sanity check, last
    return Fmt::Unknown;
}

Fmt fromExtension(const QString &p) {
    QString e = QFileInfo(p).suffix().toLower();
    if (e.isEmpty()) e = p.toLower();
    if (e.startsWith(QLatin1Char('.'))) e = e.mid(1);
    if (e == QLatin1String("png")) return Fmt::PNG;
    for (auto &i : kFormats) if (splitExts(i.exts).contains(e)) return i.f;
    return Fmt::Unknown;
}

static bool readViaQt(const QByteArray &data, ImageSeq &out, QString *err, const char *fmt = nullptr) {
    QBuffer buf;
    buf.setData(data);
    buf.open(QIODevice::ReadOnly);
    QImageReader r(&buf, fmt);
    r.setAutoTransform(true);
    QImage img;
    out.frames.clear();
    while (r.read(&img)) {
        out.frames.append({img.convertToFormat(QImage::Format_ARGB32), r.nextImageDelay() > 0 ? r.nextImageDelay() : 100});
        if (!r.supportsAnimation() || !r.jumpToNextImage()) break;
    }
    if (out.frames.isEmpty()) { codec::setErr(err, QStringLiteral("Could not decode image: %1").arg(r.errorString())); return false; }
    out.width = out.frames[0].image.width();
    out.height = out.frames[0].image.height();
    return true;
}

// ---- HDR tone mapping
ToneMap toneMap() {
    QSettings s;
    ToneMap t;
    t.op = qBound(0, s.value(QStringLiteral("import/hdrToneMap"), int(ToneMap::ACES)).toInt(), 3);
    t.exposure = qBound(-10.0, s.value(QStringLiteral("import/hdrExposure"), 0.0).toDouble(), 10.0);
    return t;
}
void setToneMap(const ToneMap &t) {
    QSettings s;
    s.setValue(QStringLiteral("import/hdrToneMap"), t.op);
    s.setValue(QStringLiteral("import/hdrExposure"), t.exposure);
}
QImage applyToneMap(const HdrImage &h, const ToneMap &t) {
    if (h.isNull() || h.rgba.size() < qsizetype(h.width) * h.height * 4) return QImage();
    QImage out(h.width, h.height, QImage::Format_ARGB32);
    double scale = std::pow(2.0, t.exposure);
    if (t.op == ToneMap::Exposure) {
        // auto exposure: map the log-average luminance to middle grey (0.18)
        double sum = 0; qint64 n = 0;
        for (qint64 i = 0; i < qint64(h.width) * h.height; ++i) {
            const float *p = h.rgba.constData() + i * 4;
            const double L = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            if (std::isfinite(L)) { sum += std::log(1e-6 + qMax(0.0, L)); ++n; }
        }
        const double avg = n ? std::exp(sum / n) : 0.18;
        scale *= 0.18 / qMax(1e-6, avg);
    }
    auto curve = [&](double v) -> double {
        if (!std::isfinite(v) || v < 0) v = 0;
        v *= scale;
        switch (t.op) {
        case ToneMap::Reinhard: return v / (1.0 + v);
        case ToneMap::ACES: { const double a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14; return qBound(0.0, (v * (a * v + b)) / (v * (c * v + d) + e), 1.0); }
        default: return qBound(0.0, v, 1.0);
        }
    };
    for (int y = 0; y < h.height; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const float *p = h.rgba.constData() + qint64(y) * h.width * 4;
        for (int x = 0; x < h.width; ++x, p += 4) {
            auto ch = [&](float v) { return int(std::lround(codec::linearToSrgb(float(curve(v))) * 255.0)); };
            const float a = std::isfinite(p[3]) ? qBound(0.0f, p[3], 1.0f) : 1.0f;
            d[x] = qRgba(qBound(0, ch(p[0]), 255), qBound(0, ch(p[1]), 255), qBound(0, ch(p[2]), 255), int(std::lround(a * 255)));
        }
    }
    return out;
}
bool readHdr(const QByteArray &data, HdrImage &out, QString *err) {
    switch (detect(data)) {
    case Fmt::EXR: return codec::readExr(data, out, err);
    case Fmt::HDR: return codec::readRgbe(data, out, err);
    case Fmt::PFM: return codec::readPfm(data, out, err);
    default: codec::setErr(err, QStringLiteral("Not an HDR image (EXR / HDR / PFM)")); return false;
    }
}

static bool readLayered(const QByteArray &data, Fmt f, ImageSeq &out, QString *err) {
    codec::LayeredImage li;
    const bool ok = f == Fmt::PSD ? codec::readPsd(data, li, err) : f == Fmt::ORA ? codec::readOra(data, li, err) : codec::readXcf(data, li, err);
    if (!ok) return false;
    QImage img = li.composite.isNull() ? codec::compositeLayers(li) : li.composite;
    if (img.isNull()) { codec::setErr(err, QStringLiteral("%1: empty image").arg(formatName(f))); return false; }
    out.frames = {AnimFrame{img.convertToFormat(QImage::Format_ARGB32), 100}};
    return true;
}

static bool readAs(const QByteArray &data, Fmt f, ImageSeq &out, QString *err) {
    switch (f) {
    case Fmt::APNG: return codec::readApng(data, out, err);
    case Fmt::GIF: return codec::readGif(data, out, err);
    case Fmt::WEBP: return codec::readWebp(data, out, err);
    case Fmt::AVIF: return codec::readAvif(data, out, err);
    case Fmt::JXL: return codec::readJxl(data, out, err);
    case Fmt::HEIC: return codec::readHeif(data, out, err);
    case Fmt::RAW: return codec::readRaw(data, out, err);
    case Fmt::PBM: case Fmt::PGM: case Fmt::PPM: return codec::readNetpbm(data, out, err);
    case Fmt::PAM: return codec::readPam(data, out, err);
    case Fmt::TIFF: return codec::readTiff(data, out, err);
    case Fmt::TGA: return codec::readTga(data, out, err);
    case Fmt::BMP: return codec::readBmp(data, out, err);
    case Fmt::QOI: return codec::readQoi(data, out, err);
    case Fmt::ICO: case Fmt::CUR: return codec::readIco(data, out, err);
    case Fmt::ICNS: return codec::readIcns(data, out, err);
    case Fmt::PCX: return codec::readPcx(data, out, err);
    case Fmt::DDS: return codec::readDds(data, out, err);
    case Fmt::KTX2: return codec::readKtx2(data, out, err);
    case Fmt::PSD: case Fmt::ORA: case Fmt::XCF: return readLayered(data, f, out, err);
    case Fmt::SVG: case Fmt::SVGZ: return codec::rasterizeSvg(data, out, err);
    case Fmt::EXR: case Fmt::HDR: case Fmt::PFM: {
        HdrImage h;
        if (!readHdr(data, h, err)) return false;
        out.frames = {AnimFrame{applyToneMap(h, toneMap()), 100}};
        return true;
    }
    case Fmt::PDF: codec::setErr(err, QStringLiteral("PDF import is not supported (%1 writes PDF only)").arg(QStringLiteral(LW_PRODUCT_NAME))); return false;
    case Fmt::XPM: return readViaQt(data, out, err, "xpm");
    case Fmt::XBM: return readViaQt(data, out, err, "xbm");
    default: return readViaQt(data, out, err);
    }
}

static bool finish(bool ok, ImageSeq &out) {
    if (ok && !out.frames.isEmpty()) {
        out.width = out.frames[0].image.width();
        out.height = out.frames[0].image.height();
        for (auto &f : out.frames)
            if (f.image.format() != QImage::Format_ARGB32 && f.image.format() != QImage::Format_ARGB32_Premultiplied)
                f.image = f.image.convertToFormat(QImage::Format_ARGB32);
    }
    return ok && !out.frames.isEmpty() && !out.frames[0].image.isNull();
}

bool readData(const QByteArray &data, ImageSeq &out, QString *err) {
    out = ImageSeq();
    return finish(readAs(data, detect(data), out, err), out);
}

bool read(const QString &path, ImageSeq &out, QString *err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { codec::setErr(err, QStringLiteral("Cannot open %1: %2").arg(path, f.errorString())); return false; }
    const QByteArray data = f.readAll();
    Fmt fmt = detect(data);
    const Fmt ext = fromExtension(path);
    // formats without a reliable magic number, and TIFF-based camera RAW files (NEF, ARW, DNG ...)
    if (fmt == Fmt::Unknown && ext != Fmt::Unknown) fmt = ext;
    else if (fmt == Fmt::TIFF && ext == Fmt::RAW) fmt = Fmt::RAW;
    else if ((fmt == Fmt::ICO || fmt == Fmt::CUR) && (ext == Fmt::TGA)) fmt = Fmt::TGA;
    out = ImageSeq();
    return finish(readAs(data, fmt, out, err), out);
}

static bool writeViaQt(QByteArray &out, const QImage &img, const char *fmt, int quality, QString *err) {
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QImageWriter w(&buf, fmt);
    if (quality >= 0) w.setQuality(quality);
    if (!w.write(img)) { codec::setErr(err, w.errorString()); return false; }
    return true;
}

static QByteArray pngBytes(const QImage &img) {
    QByteArray b;
    ExportOptions po;
    codec::writePng(b, img, po, nullptr);
    return b;
}

static bool writeFlatSvg(QByteArray &out, const QImage &img) {
    out = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                         "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n"
                         "  <image width=\"%1\" height=\"%2\" xlink:href=\"data:image/png;base64,")
              .arg(img.width()).arg(img.height()).toUtf8();
    out += pngBytes(img).toBase64();
    out += "\"/>\n</svg>\n";
    return true;
}

static bool writeFlatPdf(QByteArray &out, const QImage &img, QString *err) {
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QPdfWriter pdf(&buf);
    pdf.setResolution(72);
    pdf.setCreator(QStringLiteral("%1 %2").arg(QStringLiteral(LW_PRODUCT_NAME), QStringLiteral(LW_VERSION)));
    pdf.setPageMargins(QMarginsF(0, 0, 0, 0));
    pdf.setPageSize(QPageSize(QSizeF(img.width(), img.height()), QPageSize::Point, QString(), QPageSize::ExactMatch));
    QPainter p;
    if (!p.begin(&pdf)) { codec::setErr(err, QStringLiteral("PDF: could not start the PDF writer")); return false; }
    p.drawImage(QRect(0, 0, img.width(), img.height()), img);
    p.end();
    return true;
}

ImageSeq cropSeq(const ImageSeq &seq, const QRect &areaIn, bool *ok) {
    if (ok) *ok = true;
    if (areaIn.isEmpty() || seq.frames.isEmpty()) return seq;
    const int w = seq.width > 0 ? seq.width : seq.frames[0].image.width();
    const int h = seq.height > 0 ? seq.height : seq.frames[0].image.height();
    const QRect a = areaIn.normalized() & QRect(0, 0, w, h);
    if (a.isEmpty()) { if (ok) *ok = false; return seq; }
    if (a == QRect(0, 0, w, h)) return seq;
    ImageSeq out = seq;
    out.width = a.width();
    out.height = a.height();
    for (auto &f : out.frames) f.image = f.image.copy(a);
    return out;
}

ImageSeq scaleSeq(const ImageSeq &seq, int pct, int resample) {
    if (pct == 100 || pct <= 0 || seq.frames.isEmpty()) return seq;
    const int w = seq.width > 0 ? seq.width : seq.frames[0].image.width();
    const int h = seq.height > 0 ? seq.height : seq.frames[0].image.height();
    const QSize ns(qMax(1, qRound(w * pct / 100.0)), qMax(1, qRound(h * pct / 100.0)));
    ImageSeq out = seq;
    out.width = ns.width();
    out.height = ns.height();
    for (auto &f : out.frames) f.image = pixelscale::resample(f.image, ns, resample);
    return out;
}

// Export scale (after the area): resize the frames and the CUR hotspot
static void applyScale(ImageSeq &seq, ExportOptions &o) {
    const int pct = qBound(1, o.scalePct, 1600);
    o.scalePct = 100;
    if (pct == 100) return;
    seq = scaleSeq(seq, pct, o.scaleResample);
    o.curHotX = qBound(0, qRound(o.curHotX * pct / 100.0), qMax(0, seq.width - 1));
    o.curHotY = qBound(0, qRound(o.curHotY * pct / 100.0), qMax(0, seq.height - 1));
}

// Export Area: cut the frames and move the CUR hotspot into the area
static bool applyArea(ImageSeq &seq, ExportOptions &o, QString *err) {
    if (o.area.isEmpty()) { applyScale(seq, o); return true; }
    bool ok = true;
    seq = cropSeq(seq, o.area, &ok);
    if (!ok) { codec::setErr(err, QStringLiteral("The export area lies outside the canvas")); return false; }
    const QRect a = o.area.normalized();
    o.curHotX = qBound(0, o.curHotX - qMax(0, a.x()), qMax(0, seq.width - 1));
    o.curHotY = qBound(0, o.curHotY - qMax(0, a.y()), qMax(0, seq.height - 1));
    o.area = QRect();
    applyScale(seq, o);
    return true;
}

bool writeData(QByteArray &out, const ImageSeq &seqArea, const ExportOptions &oArea, QString *err) {
    out.clear();
    if (seqArea.frames.isEmpty()) { codec::setErr(err, QStringLiteral("Nothing to export")); return false; }
    ImageSeq seqIn = seqArea;
    ExportOptions o = oArea;
    if (!applyArea(seqIn, o, err)) return false;
    if (!canWrite(o.format)) {
        codec::setErr(err, o.format == Fmt::RAW ? QStringLiteral("Camera RAW files cannot be written")
                                                : QStringLiteral("%1 export is not available in this build").arg(formatName(o.format)));
        return false;
    }
    ImageSeq seq = seqIn;
    if (!(o.animated && canAnimate(o.format)) && seq.frames.size() > 1) seq.frames.resize(1);
    for (auto &f : seq.frames) {
        if (f.image.format() != QImage::Format_ARGB32)
            f.image = f.image.convertToFormat(QImage::Format_ARGB32);
    }
    const QImage &first = seq.frames[0].image;
    switch (o.format) {
    case Fmt::PNG:
        if (o.tinyPng) return codec::writeTinyPng(out, first, o, err);
        return codec::writePng(out, first, o, err);
    case Fmt::APNG: {
        if (!o.tinyPng) return codec::writeApng(out, seq, o, err);
        // Tiny: quantise every frame to one shared palette; like pngquant --skip-if-larger, keep the
        // plain APNG when quantising does not make the file smaller.
        ExportOptions plain = o; plain.tinyPng = false;
        if (!codec::writeApng(out, seq, plain, err)) return false;
        QVector<QImage> imgs;
        for (auto &f : seq.frames) imgs << f.image;
        if (codec::tinyQuantizeFrames(imgs, o, nullptr)) {
            ImageSeq q = seq;
            for (int i = 0; i < imgs.size(); ++i) q.frames[i].image = imgs[i];
            QByteArray t;
            if (codec::writeApng(t, q, o, nullptr) && t.size() < out.size()) out = t;
        }
        return true;
    }
    case Fmt::JPEG: return codec::writeJpeg(out, first, o, err);
    case Fmt::BMP: return codec::writeBmp(out, first, o, err);
    case Fmt::TIFF: return codec::writeTiff(out, seq, o, err);
    case Fmt::TGA: return codec::writeTga(out, first, o, err);
    case Fmt::WEBP: case Fmt::AVIF: {
        auto enc = [&](QByteArray &b, const ExportOptions &oo) {
            return o.format == Fmt::WEBP ? codec::writeWebp(b, seq, oo, err) : codec::writeAvif(b, seq, oo, err);
        };
        if (!enc(out, o)) return false;
        if (o.tinyPng && o.lossless) {
            // "smallest lossless": also try the slowest / strongest lossless effort, keep the smaller file
            ExportOptions oo = o;
            oo.quality = 100;
            if (o.format == Fmt::WEBP) oo.webpEffort = 6;
            QByteArray b;
            if (enc(b, oo) && !b.isEmpty() && b.size() < out.size()) out = b;
        }
        return true;
    }
    case Fmt::GIF: return codec::writeGif(out, seq, o, err);
    case Fmt::JXL: return codec::writeJxl(out, seq, o, err);
    case Fmt::HEIC: return codec::writeHeif(out, seq, o, err);
    case Fmt::PPM: return codec::writeNetpbm(out, codec::flatten(first, o.matte), false, o.netpbmAscii, err);
    case Fmt::PGM: return codec::writeNetpbm(out, codec::flatten(first, o.matte), true, o.netpbmAscii, err);
    case Fmt::PBM: return codec::writePbm(out, codec::flatten(first, o.matte), o.netpbmAscii, o.dither, err);
    case Fmt::PAM: return codec::writePam(out, first, o.alpha, err);
    case Fmt::PFM: return codec::writePfm(out, codec::flatten(first, o.matte), err);
    case Fmt::HDR: return codec::writeRgbe(out, codec::flatten(first, o.matte), err);
    case Fmt::EXR: return codec::writeExr(out, first, o, err);
    case Fmt::DDS: return codec::writeDds(out, first, o, err);
    case Fmt::KTX2: return codec::writeKtx2(out, first, o, err);
    case Fmt::QOI: return codec::writeQoi(out, first, o, err);
    case Fmt::ICO: return codec::writeIco(out, first, o, false, err);
    case Fmt::CUR: return codec::writeIco(out, first, o, true, err);
    case Fmt::ICNS: return codec::writeIcns(out, first, o, err);
    case Fmt::PCX: return codec::writePcx(out, first, o, err);
    case Fmt::XPM: return writeViaQt(out, first, "xpm", -1, err);
    case Fmt::XBM: {
        // 1-bit: dark pixels are set; transparent pixels count as background (matte)
        QImage g = codec::flatten(first, o.matte).convertToFormat(QImage::Format_Mono, o.dither ? Qt::MonoOnly | Qt::DiffuseDither : Qt::MonoOnly | Qt::ThresholdDither);
        return writeViaQt(out, g, "xbm", -1, err);
    }
    case Fmt::PSD: case Fmt::ORA: case Fmt::XCF: {
        codec::LayeredImage li;
        li.width = first.width();
        li.height = first.height();
        codec::RasterLayer rl;
        rl.name = QStringLiteral("Background");
        rl.image = first;
        li.layers << rl;
        li.composite = first;
        if (o.format == Fmt::PSD) return codec::writePsd(out, li, err);
        if (o.format == Fmt::ORA) return codec::writeOra(out, li, err);
        return codec::writeXcf(out, li, err);
    }
    case Fmt::SVG: return writeFlatSvg(out, first);
    case Fmt::SVGZ: {
        QByteArray svg;
        writeFlatSvg(svg, first);
        out = docio::gzip(svg);
        return !out.isEmpty();
    }
    case Fmt::PDF: return writeFlatPdf(out, first, err);
    case Fmt::CHeader: return codec::writeCHeader(out, seq, o, err);
    default: codec::setErr(err, QStringLiteral("Unsupported export format")); return false;
    }
}

static bool saveBytes(const QString &path, const QByteArray &data, QString *err) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { codec::setErr(err, f.errorString()); return false; }
    f.write(data);
    if (!f.commit()) { codec::setErr(err, f.errorString()); return false; }
    return true;
}

bool write(const QString &path, const ImageSeq &seq, const ExportOptions &oIn, QString *err) {
    QByteArray data;
    if (oIn.format == Fmt::CHeader) {
        // the identifier comes from the file name unless one was typed; NAME.bin goes next to NAME.h
        ExportOptions o = oIn;
        const QFileInfo fi(path);
        if (o.chName.trimmed().isEmpty()) o.chName = fi.completeBaseName();
        o.outputName = fi.fileName();
        ImageSeq s = seq;
        if (s.frames.isEmpty()) { codec::setErr(err, QStringLiteral("Nothing to export")); return false; }
        if (!applyArea(s, o, err)) return false;
        if (!(o.animated) && s.frames.size() > 1) s.frames.resize(1);
        for (auto &f : s.frames) f.image = f.image.convertToFormat(QImage::Format_ARGB32);
        if (s.frames.isEmpty()) { codec::setErr(err, QStringLiteral("Nothing to export")); return false; }
        codec::CHeaderData info;
        if (!codec::writeCHeader(data, s, o, err, &info)) return false;
        if (!saveBytes(path, data, err)) return false;
        if (o.chRawBin && !saveBytes(fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".bin")), info.bin, err)) return false;
        return true;
    }
    if (!writeData(data, seq, oIn, err)) return false;
    return saveBytes(path, data, err);
}

static QString globs(Fmt f) {
    QStringList g;
    for (const QString &e : extensions(f)) g << QStringLiteral("*.") + e;
    return g.join(QLatin1Char(' '));
}

QString importFilter() {
    // Both brands open both payload-compatible native document extensions.
    QStringList all;
    QStringList each;
    if (brand::isBonbon()) {
        all << QStringLiteral("*.bon") << QStringLiteral("*.lwd");
        each << brand::docFilter(); // "Bonbon Document (*.bon)"
        each << QStringLiteral("Libreworks document (*.lwd)");
    } else {
        all << QStringLiteral("*.lwd") << QStringLiteral("*.bon");
        each << brand::docFilter();
        each << QStringLiteral("Bonbon Document (*.bon)");
    }
    for (auto &i : kFormats) {
        if (!canRead(i.f) || i.f == Fmt::APNG) continue;
        const QString g = i.f == Fmt::PNG ? QStringLiteral("*.png *.apng") : globs(i.f);
        all << g;
        each << QStringLiteral("%1 (%2)").arg(i.f == Fmt::PNG ? QStringLiteral("PNG / APNG") : QString::fromLatin1(i.name), g);
    }
    return QStringLiteral("All images (%1);;").arg(all.join(QLatin1Char(' '))) + each.join(QStringLiteral(";;")) + QStringLiteral(";;All files (*)");
}

QString exportFilter(Fmt f) {
    if (f == Fmt::TIFF) return QStringLiteral("TIFF (*.tif *.tiff)");
    if (f == Fmt::JPEG) return QStringLiteral("JPEG (*.jpg *.jpeg)");
    if (f == Fmt::HEIC) return QStringLiteral("HEIC / HEIF (*.heic *.heif)");
    if (f == Fmt::CHeader) return QStringLiteral("C Header (*.h)");
    return QStringLiteral("%1 (*.%2)").arg(formatName(f), extension(f));
}

QString codecSummary() {
    QStringList l;
    for (auto &i : kFormats) {
        QString s;
        if (canRead(i.f) && canWrite(i.f)) s = QStringLiteral("read / write");
        else if (canRead(i.f)) s = QStringLiteral("read only");
        else if (canWrite(i.f)) s = QStringLiteral("write only");
        else s = QStringLiteral("not built");
        if (i.f == Fmt::RAW && canRead(i.f)) s += QStringLiteral(" (LibRaw %1)").arg(codec::rawLibraryVersion());
        if (i.f == Fmt::JPEG) {
            QStringList enc{QStringLiteral("libjpeg-turbo")};
            if (codec::jpegEncoderAvailable(1)) enc << QStringLiteral("MozJPEG %1").arg(codec::jpegEncoderVersion(1));
            if (codec::jpegEncoderAvailable(2)) enc << QStringLiteral("jpegli %1").arg(codec::jpegEncoderVersion(2));
            s += QStringLiteral(" (encoders: %1)").arg(enc.join(QStringLiteral(", ")));
        }
        if (i.f == Fmt::PNG) s += codec::tinyAvailable() ? QStringLiteral(" (Tiny compression: libimagequant + zopfli)") : QStringLiteral(" (Tiny compression: not built)");
        l << QStringLiteral("%1: %2").arg(QString::fromLatin1(i.name), s);
    }
    return l.join('\n');
}

} // namespace ImageIO

namespace codec {
QImage flatten(const QImage &img, const QColor &matte) {
    QImage out(img.size(), QImage::Format_ARGB32);
    out.fill(matte.isValid() ? QColor(matte.red(), matte.green(), matte.blue()) : QColor(Qt::white));
    QPainter p(&out);
    p.drawImage(0, 0, img);
    return out;
}
}
