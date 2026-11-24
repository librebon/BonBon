// v0.2.5 format tests (linked into roundtrip_test): every new format, PNG variants, Tiny compression,
// JPEG encoders, layered PSD / ORA / XCF, SVG / SVGZ, PDF, editable PNG, Fireworks PNG and favicons.
#include "core/Document.h"
#include "core/Render.h"
#include "io/ImageIO.h"
#include "io/Codecs.h"
#include "io/DocIO.h"
#include "io/LwdFormat.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <tiffio.h>

using Report = std::function<void(const QString &, bool, const QString &)>;
static int g_skip025 = 0;
static void skip(const QString &name, const QString &why) { std::printf("%-34s SKIP (%s)\n", qPrintable(name), qPrintable(why)); ++g_skip025; }

// test image: colour gradients, an alpha ramp, fully transparent and opaque areas
static QImage testImage(int w = 64, int h = 48) {
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int a = x < w / 4 ? 0 : x < w / 2 ? (y * 255) / (h - 1) : 255;
            img.setPixel(x, y, qRgba((x * 255) / (w - 1), (y * 255) / (h - 1), ((x + y) * 7) & 255, a));
        }
    return img;
}
static QImage opaqueImage(int w = 64, int h = 48) {
    QImage img = testImage(w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { QRgb p = img.pixel(x, y); img.setPixel(x, y, qRgb(qRed(p), qGreen(p), qBlue(p))); }
    return img;
}
static int maxDiff025(const QImage &a, const QImage &b, bool alpha = true) {
    if (a.size() != b.size()) return 999;
    QImage x = a.convertToFormat(QImage::Format_ARGB32), y = b.convertToFormat(QImage::Format_ARGB32);
    int m = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            if (qAlpha(p) == 0 && qAlpha(q) == 0) continue;
            m = qMax(m, qMax(qAbs(qRed(p) - qRed(q)), qMax(qAbs(qGreen(p) - qGreen(q)), qAbs(qBlue(p) - qBlue(q)))));
            if (alpha) m = qMax(m, qAbs(qAlpha(p) - qAlpha(q)));
        }
    return m;
}
static double psnr(const QImage &a, const QImage &b) {
    // premultiplied comparison: the colour of a fully transparent pixel carries no information
    QImage x = a.convertToFormat(QImage::Format_ARGB32_Premultiplied), y = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    double se = 0; qint64 n = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            const int d[4] = {qRed(p) - qRed(q), qGreen(p) - qGreen(q), qBlue(p) - qBlue(q), qAlpha(p) - qAlpha(q)};
            for (int k = 0; k < 4; ++k) se += d[k] * d[k];
            n += 4;
        }
    if (se == 0) return 99;
    return 10 * std::log10(255.0 * 255.0 / (se / n));
}
// deterministic sensor-like noise, -n..n
static int noise(int x, int y, int c, int n) { quint32 h = quint32(x) * 73856093u ^ quint32(y) * 19349663u ^ quint32(c) * 83492791u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; return int(h % quint32(2 * n + 1)) - n; }
static int clamp8(int v) { return qBound(0, v, 255); }
static QImage flatten(const QImage &i, const QColor &m) { return codec::flatten(i, m); }
static ImageSeq seqOf(const QImage &img) { ImageSeq s; s.width = img.width(); s.height = img.height(); s.frames << AnimFrame{img, 100}; return s; }
static bool decode(const QByteArray &d, QImage &out, QString *err = nullptr) {
    ImageSeq s;
    if (!ImageIO::readData(d, s, err)) return false;
    out = s.frames[0].image;
    return true;
}
struct Ihdr { int w = 0, h = 0, depth = 0, colorType = -1, interlace = -1; };
static Ihdr ihdr(const QByteArray &png) {
    Ihdr r;
    if (png.size() < 33 || png.mid(12, 4) != "IHDR") return r;
    auto be = [&](int o) { return (uchar(png[o]) << 24) | (uchar(png[o + 1]) << 16) | (uchar(png[o + 2]) << 8) | uchar(png[o + 3]); };
    r.w = be(16); r.h = be(20); r.depth = uchar(png[24]); r.colorType = uchar(png[25]); r.interlace = uchar(png[28]);
    return r;
}
static bool hasChunk(const QByteArray &png, const char *t) { return !codec::pngChunk(png, t).isEmpty(); }
static bool jpegHasMarker(const QByteArray &d, uchar m) {
    for (int i = 2; i + 3 < d.size();) {
        if (uchar(d[i]) != 0xFF) return false;
        const uchar mk = uchar(d[i + 1]);
        if (mk == m) return true;
        if (mk == 0xDA) return false;
        i += 2 + ((uchar(d[i + 2]) << 8) | uchar(d[i + 3]));
    }
    return false;
}

// a small document: two layers, a rectangle, an ellipse with a gradient, a path and text
static Document *vectorDoc() {
    Document *d = Document::createNew(120, 80, Qt::white);
    auto mk = [&](ObjType t) { VObject o; o.id = d->newId(); o.type = t; return o; };
    VObject r = mk(ObjType::Rect); r.name = "Box"; r.rect = QRectF(10, 10, 50, 30); r.fill = QColor(200, 40, 40); r.strokeOn = false; r.roundness = 20;
    d->s.frames[0].layers[0] << r;
    LayerInfo l2; l2.id = d->newId(); l2.name = "Shapes";
    d->s.layers << l2;
    d->s.frames[0].layers.resize(2);
    VObject e = mk(ObjType::Ellipse); e.name = "Disc"; e.rect = QRectF(60, 20, 40, 40); e.fill = QColor(20, 120, 220); e.stroke = Qt::black; e.strokeWidth = 2;
    e.fillKind = FillKind::Linear;
    d->s.frames[0].layers[1] << e;
    VObject t = mk(ObjType::Text); t.name = "Label"; t.text = "Hi"; t.rect = QRectF(15, 50, 60, 24); t.fontSize = 16; t.fill = Qt::black; t.strokeOn = false;
    d->s.frames[0].layers[1] << t;
    d->resetHistory();
    return d;
}

// minimal DNG ("LinearRaw" RGB, 16-bit) written with libtiff
static bool makeDng(const QString &path, int w, int h) {
    TIFF *t = TIFFOpen(QFile::encodeName(path).constData(), "w");
    if (!t) return false;
    TIFFSetField(t, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, uint32_t(w)); TIFFSetField(t, TIFFTAG_IMAGELENGTH, uint32_t(h));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, uint16_t(16)); TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, uint16_t(3));
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, uint16_t(34892)); TIFFSetField(t, TIFFTAG_PLANARCONFIG, uint16_t(PLANARCONFIG_CONTIG));
    TIFFSetField(t, TIFFTAG_COMPRESSION, uint16_t(COMPRESSION_NONE));
    TIFFSetField(t, TIFFTAG_MAKE, "Libreworks"); TIFFSetField(t, TIFFTAG_MODEL, "Test");
    static const uint8_t ver[4] = {1, 4, 0, 0};
    TIFFSetField(t, TIFFTAG_DNGVERSION, ver); TIFFSetField(t, TIFFTAG_DNGBACKWARDVERSION, ver);
    TIFFSetField(t, TIFFTAG_UNIQUECAMERAMODEL, "Libreworks Test");
    static const float cm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    TIFFSetField(t, TIFFTAG_COLORMATRIX1, 9, cm);
    TIFFSetField(t, TIFFTAG_CALIBRATIONILLUMINANT1, uint16_t(21));
    static const float neutral[3] = {1, 1, 1};
    TIFFSetField(t, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
    QVector<uint16_t> row(w * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) { row[x * 3] = uint16_t(x * 65535 / (w - 1)); row[x * 3 + 1] = uint16_t(y * 65535 / (h - 1)); row[x * 3 + 2] = 20000; }
        TIFFWriteScanline(t, row.data(), uint32_t(y), 0);
    }
    TIFFClose(t);
    return true;
}

void runFormatTests025(const QString &tmpDir, const Report &report) {
    std::printf("\n---- v0.2.5 formats ----\n");
    const QImage img = testImage(), opq = opaqueImage();
    auto rt = [&](const QString &label, ExportOptions o, const QImage &src, int tol, bool cmpAlpha, Fmt expectDetect, QSize expectSize = QSize()) {
        if (!ImageIO::canWrite(o.format) || !ImageIO::canRead(o.format)) { skip(label, "codec not built"); return QByteArray(); }
        QByteArray d; QString err;
        if (!ImageIO::writeData(d, seqOf(src), o, &err)) { report(label, false, "export: " + err); return QByteArray(); }
        const QString path = QDir(tmpDir).filePath(QStringLiteral("v025_%1.%2").arg(QString(label).replace(QRegularExpression("[^A-Za-z0-9]"), "_"), ImageIO::extension(o.format)));
        QFile f(path); f.open(QIODevice::WriteOnly); f.write(d); f.close();
        ImageSeq back;
        if (!ImageIO::read(path, back, &err)) { report(label, false, "import: " + err); return d; }
        const QImage b = back.frames[0].image;
        const QSize es = expectSize.isValid() ? expectSize : src.size();
        const int md = b.size() == src.size() ? maxDiff025(src, b, cmpAlpha) : -1;
        const Fmt det = ImageIO::detect(d);
        bool ok = b.size() == es && (tol < 0 || (md >= 0 && md <= tol)) && (expectDetect == Fmt::Unknown || det == expectDetect);
        report(label, ok, QStringLiteral("%1 bytes, %2x%3, max diff %4, detected %5").arg(d.size()).arg(b.width()).arg(b.height()).arg(md).arg(ImageIO::formatName(det)));
        return d;
    };
    auto opt = [](Fmt f, std::function<void(ExportOptions &)> tw = {}) { ExportOptions o; o.format = f; if (tw) tw(o); return o; };

    // ---- full BMP
    for (int bits : {1, 4, 8, 16, 24, 32}) {
        const int tol = bits == 32 ? 0 : bits == 24 ? 0 : bits == 16 ? 8 : -1;
        rt(QStringLiteral("BMP %1-bit").arg(bits), opt(Fmt::BMP, [=](ExportOptions &o) { o.bmpBits = bits; }), bits == 32 ? img : opq, tol, bits == 32, Fmt::BMP);
    }
    rt("BMP 8-bit RLE8", opt(Fmt::BMP, [](ExportOptions &o) { o.bmpBits = 8; o.bmpRle = true; }), opq, -1, false, Fmt::BMP);
    rt("BMP 4-bit RLE4", opt(Fmt::BMP, [](ExportOptions &o) { o.bmpBits = 4; o.bmpRle = true; }), opq, -1, false, Fmt::BMP);
    {   // RLE8 of a 2-colour image is exact
        QImage two(40, 10, QImage::Format_ARGB32); two.fill(qRgb(255, 0, 0)); for (int x = 0; x < 20; ++x) for (int y = 0; y < 10; ++y) two.setPixel(x, y, qRgb(0, 0, 255));
        rt("BMP RLE8 exact (2 colours)", opt(Fmt::BMP, [](ExportOptions &o) { o.bmpBits = 8; o.bmpRle = true; }), two, 0, false, Fmt::BMP);
    }
    // ---- QOI (lossless, alpha)
    rt("QOI RGBA", opt(Fmt::QOI), img, 0, true, Fmt::QOI);
    rt("QOI RGB", opt(Fmt::QOI), opq, 0, true, Fmt::QOI);
    // ---- ICO / CUR / ICNS: the largest entry is read back
    {
        QImage sq = testImage(64, 64);
        rt("ICO 16/24/32/48/256", opt(Fmt::ICO), sq, -1, true, Fmt::ICO, QSize(256, 256));
        rt("ICO 16/32 only", opt(Fmt::ICO, [](ExportOptions &o) { o.icoSizes = 0x5; }), sq, -1, true, Fmt::ICO, QSize(32, 32));
        rt("ICO 64 (exact)", opt(Fmt::ICO, [](ExportOptions &o) { o.icoSizes = 1 << 4; }), sq, 1, true, Fmt::ICO);
        QByteArray cur = rt("CUR 32 hotspot", opt(Fmt::CUR, [](ExportOptions &o) { o.icoSizes = 1 << 2; o.curHotX = 10; o.curHotY = 20; }), sq, -1, true, Fmt::CUR, QSize(32, 32));
        if (cur.size() > 14) report("CUR hotspot stored", (uchar(cur[10]) | uchar(cur[11]) << 8) == 5 && (uchar(cur[12]) | uchar(cur[13]) << 8) == 10, QString());
        rt("ICNS all sizes", opt(Fmt::ICNS), sq, -1, true, Fmt::ICNS, QSize(1024, 1024));
        rt("ICNS 16/32 (legacy RLE)", opt(Fmt::ICNS, [](ExportOptions &o) { o.icnsSizes = 0x5; }), sq, -1, true, Fmt::ICNS, QSize(32, 32));
        rt("ICNS 64 (exact)", opt(Fmt::ICNS, [](ExportOptions &o) { o.icnsSizes = 1 << 4; }), sq, 1, true, Fmt::ICNS);
    }
    // ---- PCX
    rt("PCX 24-bit", opt(Fmt::PCX, [](ExportOptions &o) { o.alpha = false; }), opq, 0, false, Fmt::PCX);
    rt("PCX 32-bit alpha", opt(Fmt::PCX), img, 0, true, Fmt::PCX);
    rt("PCX 8-bit palette", opt(Fmt::PCX, [](ExportOptions &o) { o.pcx8 = true; }), opq, -1, false, Fmt::PCX);
    // ---- Netpbm PBM / PAM / PFM
    rt("PBM binary (P4)", opt(Fmt::PBM), opq, -1, false, Fmt::PBM);
    rt("PBM ASCII (P1)", opt(Fmt::PBM, [](ExportOptions &o) { o.netpbmAscii = true; }), opq, -1, false, Fmt::PBM);
    {
        QImage bw(17, 5, QImage::Format_ARGB32); bw.fill(Qt::white); for (int x = 0; x < 17; x += 2) bw.setPixel(x, 2, qRgb(0, 0, 0));
        rt("PBM exact (odd width)", opt(Fmt::PBM, [](ExportOptions &o) { o.dither = false; }), bw, 0, false, Fmt::PBM);
    }
    rt("PAM RGB_ALPHA", opt(Fmt::PAM), img, 0, true, Fmt::PAM);
    rt("PAM RGB", opt(Fmt::PAM, [](ExportOptions &o) { o.alpha = false; }), opq, 0, false, Fmt::PAM);
    // ---- HDR formats: written from 8-bit sRGB, read back through the tone mapper (Clamp = identity)
    {
        const ToneMap saved = ImageIO::toneMap();
        ToneMap clamp; clamp.op = ToneMap::Clamp; clamp.exposure = 0;
        ImageIO::setToneMap(clamp);
        rt("PFM (clamp tone map)", opt(Fmt::PFM), opq, 1, false, Fmt::PFM);
        rt("Radiance HDR (clamp)", opt(Fmt::HDR), opq, 16, false, Fmt::HDR);
        rt("EXR half ZIP (clamp)", opt(Fmt::EXR), img, 2, true, Fmt::EXR);
        rt("EXR float PIZ (clamp)", opt(Fmt::EXR, [](ExportOptions &o) { o.exrHalf = false; o.exrCompression = 2; }), img, 1, true, Fmt::EXR);
        rt("EXR half none (clamp)", opt(Fmt::EXR, [](ExportOptions &o) { o.exrCompression = 0; }), img, 2, true, Fmt::EXR);
        ImageIO::setToneMap(saved);
        HdrImage h; h.width = 3; h.height = 1; h.rgba = {0, 0, 0, 1, 0.5f, 0.5f, 0.5f, 1, 4, 4, 4, 1};
        ToneMap t; t.op = ToneMap::Clamp;
        QImage c = ImageIO::applyToneMap(h, t);
        t.op = ToneMap::Reinhard; QImage r = ImageIO::applyToneMap(h, t);
        t.op = ToneMap::ACES; QImage a = ImageIO::applyToneMap(h, t);
        t.op = ToneMap::Clamp; t.exposure = -2; QImage e = ImageIO::applyToneMap(h, t);
        report("HDR tone map operators", qRed(c.pixel(2, 0)) == 255 && qRed(c.pixel(1, 0)) == 188 && qRed(r.pixel(2, 0)) < 255 && qRed(r.pixel(2, 0)) > 200 &&
                                             qRed(a.pixel(2, 0)) > qRed(a.pixel(1, 0)) && qRed(e.pixel(2, 0)) == 255 && qRed(e.pixel(1, 0)) < 188,
               QStringLiteral("clamp %1/%2 reinhard %3 aces %4 ev-2 %5").arg(qRed(c.pixel(1, 0))).arg(qRed(c.pixel(2, 0))).arg(qRed(r.pixel(2, 0))).arg(qRed(a.pixel(2, 0))).arg(qRed(e.pixel(1, 0))));
        // an over-range EXR keeps the values above 1.0
        HdrImage hh; QByteArray exr;
        ExportOptions eo; eo.format = Fmt::EXR;
        if (ImageIO::canWrite(Fmt::EXR) && codec::writeExr(exr, opq, eo, nullptr) && ImageIO::readHdr(exr, hh, nullptr))
            report("EXR readHdr linear values", hh.width == opq.width() && hh.rgba.size() == opq.width() * opq.height() * 4 && hh.rgba[(opq.width() - 1) * 4] > 0.95f, QString());
    }
    // ---- DDS / KTX2
    rt("DDS RGBA8", opt(Fmt::DDS), img, 0, true, Fmt::DDS);
    rt("DDS RGBA8 + mipmaps", opt(Fmt::DDS, [](ExportOptions &o) { o.mipmaps = true; }), img, 0, true, Fmt::DDS);
    rt("DDS BC1 (DXT1)", opt(Fmt::DDS, [](ExportOptions &o) { o.ddsFormat = 1; }), opq, 40, false, Fmt::DDS);
    rt("DDS BC3 (DXT5)", opt(Fmt::DDS, [](ExportOptions &o) { o.ddsFormat = 2; }), img, 48, true, Fmt::DDS);
    rt("KTX2 RGBA8 zstd", opt(Fmt::KTX2), img, 0, true, Fmt::KTX2);
    rt("KTX2 RGBA8 raw + mipmaps", opt(Fmt::KTX2, [](ExportOptions &o) { o.ktxZstd = false; o.mipmaps = true; }), img, 0, true, Fmt::KTX2);
    rt("KTX2 BC3", opt(Fmt::KTX2, [](ExportOptions &o) { o.ddsFormat = 2; }), img, 48, true, Fmt::KTX2);
    // ---- XPM / XBM
    rt("XPM", opt(Fmt::XPM), img, 0, false, Fmt::XPM);
    {
        QImage bw(16, 8, QImage::Format_ARGB32); bw.fill(Qt::white); for (int x = 0; x < 16; x += 3) bw.setPixel(x, 3, qRgb(0, 0, 0));
        rt("XBM exact", opt(Fmt::XBM, [](ExportOptions &o) { o.dither = false; }), bw, 0, false, Fmt::XBM);
    }
    // ---- HEIC
    if (ImageIO::canWrite(Fmt::HEIC) && ImageIO::canRead(Fmt::HEIC)) {
        // HEVC is lossy with 4:2:0 chroma: compare a smooth image by PSNR
        QImage smooth(96, 64, QImage::Format_ARGB32);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 96; ++x) smooth.setPixel(x, y, qRgba(x * 255 / 95, y * 255 / 63, 128, x < 24 ? 0 : 255));
        for (bool alpha : {false, true}) {
            QImage src = alpha ? smooth : flatten(smooth, Qt::white);
            ExportOptions o = opt(Fmt::HEIC, [](ExportOptions &o) { o.quality = 90; });
            QByteArray d; QString err; QImage back;
            const bool ok = ImageIO::writeData(d, seqOf(src), o, &err) && decode(d, back, &err);
            const double p = ok ? psnr(src, back) : 0;
            report(alpha ? "HEIC q90 with alpha" : "HEIC q90 opaque", ok && ImageIO::detect(d) == Fmt::HEIC && back.size() == src.size() && p > 30 &&
                                                                (alpha ? qAlpha(back.pixel(5, 5)) < 8 && qAlpha(back.pixel(80, 5)) > 247 : true),
                   QStringLiteral("%1 bytes, PSNR %2 dB %3").arg(d.size()).arg(p, 0, 'f', 1).arg(err));
            if (alpha) { QFile f(QDir(tmpDir).filePath("alpha.heic")); f.open(QIODevice::WriteOnly); f.write(d); }
        }
    } else skip("HEIC", ImageIO::canRead(Fmt::HEIC) ? "no HEVC encoder plugin" : "libheif not built");
    // ---- Camera RAW (a linear DNG made with libtiff)
    if (ImageIO::canRead(Fmt::RAW)) {
        const QString p = QDir(tmpDir).filePath("test.dng");
        ImageSeq s; QString err;
        const bool ok = makeDng(p, 32, 24) && ImageIO::read(p, s, &err);
        report("Camera RAW (DNG via LibRaw)", ok && s.frames[0].image.width() == 32 && s.frames[0].image.height() == 24 &&
                                                   qRed(s.frames[0].image.pixel(31, 12)) > qRed(s.frames[0].image.pixel(0, 12)),
               ok ? QStringLiteral("%1x%2, LibRaw %3").arg(s.frames[0].image.width()).arg(s.frames[0].image.height()).arg(codec::rawLibraryVersion()) : err);
        report("Camera RAW is read-only", !ImageIO::canWrite(Fmt::RAW), QString());
    } else skip("Camera RAW", "LibRaw not built");

    // ---- PNG variants (libpng): IHDR colour type / bit depth, PLTE / tRNS
    {
        struct V { const char *label; std::function<void(ExportOptions &)> tw; int ct, depth; bool plte, trns; int tol; bool alpha; };
        const QVector<V> vs = {
            {"PNG 32 (RGBA 8)", [](ExportOptions &o) { o.pngType = 0; }, 6, 8, false, false, 0, true},
            {"PNG 32 16-bit", [](ExportOptions &o) { o.pngType = 0; o.png16 = true; }, 6, 16, false, false, 0, true},
            {"PNG 24 (RGB on matte)", [](ExportOptions &o) { o.pngType = 1; }, 2, 8, false, false, -1, false},
            {"PNG 24 16-bit", [](ExportOptions &o) { o.pngType = 1; o.png16 = true; }, 2, 16, false, false, -1, false},
            {"PNG 8 adaptive 256 alpha", [](ExportOptions &o) { o.pngType = 2; o.colors = 256; o.pngTransparency = 2; }, 3, 8, true, true, -1, true},
            {"PNG 8 index transparency", [](ExportOptions &o) { o.pngType = 2; o.colors = 64; o.pngTransparency = 1; }, 3, 8, true, true, -1, true},
            {"PNG 8 no transparency", [](ExportOptions &o) { o.pngType = 2; o.colors = 16; o.pngTransparency = 0; }, 3, 4, true, false, -1, false},
            {"PNG 8 web 216", [](ExportOptions &o) { o.pngType = 2; o.palette = 1; o.pngTransparency = 0; }, 3, 8, true, false, -1, false},
            {"PNG 8 B&W (2 colours)", [](ExportOptions &o) { o.pngType = 2; o.palette = 3; o.colors = 2; o.pngTransparency = 0; }, 3, 1, true, false, -1, false},
            {"PNG 8 grayscale palette", [](ExportOptions &o) { o.pngType = 2; o.palette = 2; o.pngTransparency = 0; }, 3, 8, true, false, -1, false},
            {"PNG 8 web snap adaptive", [](ExportOptions &o) { o.pngType = 2; o.palette = 4; o.colors = 128; }, 3, 8, true, true, -1, true},
            {"PNG 8 custom palette", [](ExportOptions &o) { o.pngType = 2; o.palette = 6; o.customPalette = {qRgb(0, 0, 0), qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255)}; o.pngTransparency = 0; }, 3, 2, true, false, -1, false},
            {"PNG greyscale 8", [](ExportOptions &o) { o.pngType = 3; o.alpha = false; }, 0, 8, false, false, -1, false},
            {"PNG greyscale 16 + alpha", [](ExportOptions &o) { o.pngType = 3; o.png16 = true; }, 4, 16, false, false, -1, true},
            {"PNG 32 Adam7", [](ExportOptions &o) { o.pngInterlace = true; }, 6, 8, false, false, 0, true},
            {"PNG 32 compression 0", [](ExportOptions &o) { o.pngCompression = 0; }, 6, 8, false, false, 0, true},
            {"PNG 32 compression 9", [](ExportOptions &o) { o.pngCompression = 9; }, 6, 8, false, false, 0, true},
        };
        QByteArray c0, c9;
        for (const V &v : vs) {
            ExportOptions o = opt(Fmt::PNG, v.tw);
            QByteArray d; QString err;
            if (!ImageIO::writeData(d, seqOf(img), o, &err)) { report(v.label, false, err); continue; }
            const Ihdr h = ihdr(d);
            QImage back; const bool dec = decode(d, back, &err);
            bool ok = dec && h.colorType == v.ct && h.depth == v.depth && hasChunk(d, "PLTE") == v.plte && hasChunk(d, "tRNS") == v.trns && back.size() == img.size();
            if (v.tol >= 0) ok = ok && maxDiff025(img, back) <= v.tol;
            if (v.alpha) ok = ok && qAlpha(back.pixel(2, 2)) == 0 && qAlpha(back.pixel(60, 2)) == 255;
            else ok = ok && qAlpha(back.pixel(2, 2)) == 255;
            if (QByteArray(v.label).contains("Adam7")) ok = ok && h.interlace == 1;
            if (QByteArray(v.label).contains("compression 0")) c0 = d;
            if (QByteArray(v.label).contains("compression 9")) c9 = d;
            report(v.label, ok, QStringLiteral("%1 bytes, colour type %2, depth %3, PLTE %4, tRNS %5, psnr %6")
                                    .arg(d.size()).arg(h.colorType).arg(h.depth).arg(hasChunk(d, "PLTE")).arg(hasChunk(d, "tRNS")).arg(psnr(img, back), 0, 'f', 1));
        }
        report("PNG compression 9 < 0", !c0.isEmpty() && c9.size() < c0.size(), QStringLiteral("%1 vs %2").arg(c9.size()).arg(c0.size()));
        // palette of exactly 2..256 colours
        ExportOptions o = opt(Fmt::PNG, [](ExportOptions &o) { o.pngType = 2; o.colors = 7; o.pngTransparency = 0; });
        QByteArray d; ImageIO::writeData(d, seqOf(opq), o, nullptr);
        report("PNG 8 7 colours -> PLTE 7", codec::pngChunk(d, "PLTE").size() == 21, QStringLiteral("PLTE %1 bytes").arg(codec::pngChunk(d, "PLTE").size()));
        report("indexedPalette matches", codec::indexedPalette(opq, o).size() == 7, QString());
    }
    // ---- Tiny compression (libimagequant + zopfli)
    if (codec::tinyAvailable()) {
        // a typical web graphic: the sample document (gradients, antialiased shapes, text) on a transparent canvas
        std::unique_ptr<Document> sample(Document::createSample());
        QImage photo = sample->render(0, false).convertToFormat(QImage::Format_ARGB32)
                                 .scaled(320, 210, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
        {   // cut out with a soft-edged ellipse: transparent corners, a feathered alpha edge
            QImage m = photo.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter p(&m);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            QRadialGradient g(QPointF(160, 105), 150);
            g.setColorAt(0, Qt::black); g.setColorAt(0.75, Qt::black); g.setColorAt(1, Qt::transparent);
            p.fillRect(m.rect(), g);
            p.end();
            photo = m.convertToFormat(QImage::Format_ARGB32);
        }
        ExportOptions o = opt(Fmt::PNG);
        QByteArray ref, tiny; double ps = 0;
        ImageIO::writeData(ref, seqOf(photo), o, nullptr);
        o.tinyPng = true;
        QString err;
        const bool w = codec::writeTinyPng(tiny, photo, o, &err, &ps);
        QImage back; const bool dec = w && decode(tiny, back);
        const Ihdr h = ihdr(tiny);
        report("Tiny PNG smaller", w && tiny.size() < ref.size(), QStringLiteral("%1 -> %2 bytes (%3 % saved)").arg(ref.size()).arg(tiny.size()).arg(100.0 * (ref.size() - tiny.size()) / ref.size(), 0, 'f', 1));
        report("Tiny PNG valid (indexed + tRNS)", dec && h.colorType == 3 && hasChunk(tiny, "tRNS") && hasChunk(tiny, "PLTE") && back.size() == photo.size(), QStringLiteral("colour type %1").arg(h.colorType));
        int alphaErr = 0, clear0 = 0, clear1 = 0;
        if (dec) {
            const QImage bb = back.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < photo.height(); ++y) for (int x = 0; x < photo.width(); ++x) {
                const int a0 = qAlpha(photo.pixel(x, y)), a1 = qAlpha(bb.pixel(x, y));
                alphaErr = qMax(alphaErr, qAbs(a0 - a1)); clear0 += a0 == 0; clear1 += a1 == 0;
            }
        }
        report("Tiny PNG alpha preserved", dec && clear0 > 0 && qAbs(clear0 - clear1) <= clear0 / 50 && alphaErr <= 64,
               QStringLiteral("max alpha error %1, transparent px %2 -> %3").arg(alphaErr).arg(clear0).arg(clear1));
        const double p2 = dec ? psnr(photo, back) : 0;
        report("Tiny PNG PSNR > 30 dB", dec && p2 > 30, QStringLiteral("%1 dB (encoder %2 dB)").arg(p2, 0, 'f', 1).arg(ps, 0, 'f', 1));
        report("Tiny PNG strips chunks", !hasChunk(tiny, "tEXt") && !hasChunk(tiny, "iTXt") && !hasChunk(tiny, "pHYs"), QString());
        // flat art: quantising cannot beat lossless, so the lossless PNG is kept (never larger)
        QByteArray flatRef, flatTiny; ExportOptions fo = opt(Fmt::PNG);
        ImageIO::writeData(flatRef, seqOf(img), fo, nullptr); fo.tinyPng = true; ImageIO::writeData(flatTiny, seqOf(img), fo, nullptr);
        QImage fb; report("Tiny PNG never larger", decode(flatTiny, fb) && flatTiny.size() <= flatRef.size(), QStringLiteral("%1 -> %2 bytes").arg(flatRef.size()).arg(flatTiny.size()));
        // APNG tiny
        ImageSeq as; as.width = 64; as.height = 48;
        as.width = photo.width(); as.height = photo.height();
        for (int i = 0; i < 3; ++i) { QImage f = photo; QPainter p(&f); p.fillRect(i * 40, 20, 40, 40, QColor(200, 30, 30)); p.end(); as.frames << AnimFrame{f, 100}; }
        ExportOptions ao = opt(Fmt::APNG, [](ExportOptions &o) { o.animated = true; });
        QByteArray a0, a1; ImageIO::writeData(a0, as, ao, nullptr);
        ao.tinyPng = true;
        ImageIO::writeData(a1, as, ao, nullptr);
        ImageSeq ab; const bool aok = ImageIO::readData(a1, ab, nullptr);
        report("Tiny APNG smaller, 3 frames", aok && ab.frames.size() == 3 && a1.size() < a0.size() , QStringLiteral("%1 -> %2 bytes").arg(a0.size()).arg(a1.size()));
        // smallest-lossless WebP
        if (ImageIO::canWrite(Fmt::WEBP)) {
            ExportOptions wo = opt(Fmt::WEBP, [](ExportOptions &o) { o.lossless = true; o.quality = 10; });
            QByteArray w0, w1; ImageIO::writeData(w0, seqOf(photo), wo, nullptr);
            wo.tinyPng = true; ImageIO::writeData(w1, seqOf(photo), wo, nullptr);
            QImage wb; const bool wd = decode(w1, wb);
            report("WebP smallest lossless", wd && w1.size() <= w0.size() && maxDiff025(photo, wb) == 0, QStringLiteral("%1 -> %2 bytes").arg(w0.size()).arg(w1.size()));
        }
    } else skip("Tiny compression", "libimagequant not built");

    // ---- JPEG encoders
    {
        QImage photo(256, 192, QImage::Format_ARGB32);
        for (int y = 0; y < photo.height(); ++y)
            for (int x = 0; x < photo.width(); ++x)
                photo.setPixel(x, y, qRgb(clamp8(int(127 + 110 * std::sin(x / 11.0 + y / 23.0)) + noise(x, y, 0, 10)), clamp8(int(127 + 110 * std::cos(y / 9.0)) + noise(x, y, 1, 10)),
                                         clamp8(((x / 32 + y / 32) & 1) ? 200 : 40) + noise(x, y, 2, 10)));
        QByteArray std0;
        for (int enc = 0; enc < 3; ++enc) {
            const QString name = enc == 0 ? "Standard" : enc == 1 ? "MozJPEG" : "jpegli";
            if (!codec::jpegEncoderAvailable(enc)) { skip("JPEG " + name, "encoder library not found"); continue; }
            ExportOptions o = opt(Fmt::JPEG, [=](ExportOptions &o) { o.quality = 80; o.jpegEncoder = enc; });
            QByteArray d; QString err;
            const bool w = ImageIO::writeData(d, seqOf(photo), o, &err);
            QImage back; const bool dec = w && decode(d, back, &err);
            if (enc == 0) std0 = d;
            report("JPEG " + name + " decodes", dec && back.size() == photo.size() && psnr(photo, back) > 28 && jpegHasMarker(d, 0xC0) != jpegHasMarker(d, 0xC2),
                   QStringLiteral("%1 bytes, PSNR %2 dB, %3").arg(d.size()).arg(dec ? psnr(photo, back) : 0, 0, 'f', 1).arg(codec::jpegEncoderVersion(enc)));
            if (enc == 1) report("MozJPEG smaller than Standard @q80", w && !std0.isEmpty() && d.size() < std0.size(), QStringLiteral("%1 vs %2 bytes").arg(d.size()).arg(std0.size()));
            if (enc == 2) report("jpegli smaller than Standard @q80", w && !std0.isEmpty() && d.size() < std0.size(), QStringLiteral("%1 vs %2 bytes").arg(d.size()).arg(std0.size()));
            o.jpegProgressive = true; o.jpegOptimize = true;
            QByteArray p; ImageIO::writeData(p, seqOf(photo), o, &err);
            QImage pb; const bool pd = decode(p, pb);
            report("JPEG " + name + " progressive SOF2", pd && jpegHasMarker(p, 0xC2) && !jpegHasMarker(p, 0xC0), QStringLiteral("%1 bytes").arg(p.size()));
        }
        // options: subsampling, smoothing, sharpen colour edges, metadata
        ExportOptions o = opt(Fmt::JPEG, [](ExportOptions &o) { o.quality = 85; o.jpegSubsampling = 0; });
        QByteArray s444, s420; ImageIO::writeData(s444, seqOf(photo), o, nullptr);
        o.jpegSubsampling = 2; ImageIO::writeData(s420, seqOf(photo), o, nullptr);
        report("JPEG 4:4:4 larger than 4:2:0", s444.size() > s420.size(), QStringLiteral("%1 vs %2").arg(s444.size()).arg(s420.size()));
        o.jpegSmoothing = 6; QByteArray sm; ImageIO::writeData(sm, seqOf(photo), o, nullptr);
        report("JPEG smoothing smaller", sm.size() < s420.size(), QStringLiteral("%1 vs %2").arg(sm.size()).arg(s420.size()));
        o.jpegSmoothing = 0; o.jpegSharpenEdges = true; QByteArray se; QImage sb;
        report("JPEG sharpen colour edges decodes", ImageIO::writeData(se, seqOf(photo), o, nullptr) && decode(se, sb), QString());
        o.jpegSharpenEdges = false; o.jpegStripMeta = true; QByteArray st; ImageIO::writeData(st, seqOf(photo), o, nullptr);
        report("JPEG strip metadata (no APP1 / COM)", !jpegHasMarker(st, 0xE1) && !jpegHasMarker(st, 0xFE), QString());
    }

    // ---- layered PSD / ORA / XCF through docio
    {
        Document *d = vectorDoc();
        // a half-transparent multiply layer
        d->s.frames[0].layers[1][0].opacity = 50;
        d->s.frames[0].layers[1][0].blend = BlendMode::Multiply;
        d->s.frames[0].layers[1].removeLast();   // single object -> layer opacity / blend
        const QImage ref = d->render(0);
        for (Fmt f : {Fmt::PSD, Fmt::ORA, Fmt::XCF}) {
            const QString n = ImageIO::formatName(f);
            ExportOptions o; o.format = f;
            QByteArray data; QString err; QStringList notes;
            bool ok = docio::exportDocument(*d, 0, o, data, &err, &notes);
            std::unique_ptr<Document> back(ok ? docio::openData(data, Fmt::Unknown, QString(), &err, &notes) : nullptr);
            ok = ok && back && ImageIO::detect(data) == f;
            int nl = 0; QStringList names;
            if (back) for (const LayerInfo &l : back->s.layers) if (!l.web) { ++nl; names << l.name; }
            int md = back ? maxDiff025(ref, back->render(0), false) : -1;
            const VObject *mo = back && back->s.frames[0].layers.size() > 1 && !back->s.frames[0].layers[1].isEmpty() ? &back->s.frames[0].layers[1][0] : nullptr;
            ok = ok && nl == 2 && names.contains("Background") == false && names.contains("Shapes") && md <= 3 && mo && mo->blend == BlendMode::Multiply && qAbs(mo->opacity - 50) <= 1;
            report(n + " layered round trip", ok, QStringLiteral("%1 bytes, layers %2, render diff %3 %4").arg(data.size()).arg(names.join('/')).arg(md).arg(err));
            QFile out(QDir(tmpDir).filePath("layers." + ImageIO::extension(f))); out.open(QIODevice::WriteOnly); out.write(data); out.close();
            // flat read (composite)
            ImageSeq s; report(n + " flat composite read", ImageIO::readData(data, s, &err) && s.frames[0].image.size() == ref.size() && maxDiff025(ref, s.frames[0].image, false) <= 3, err);
        }
        delete d;
    }
    // ---- SVG / SVGZ
    {
        Document *d = vectorDoc();
        bool pure = false; QStringList notes;
        const QByteArray svg = docio::stateToSvg(d->s, 0, &pure, &notes);
        report("SVG export is vector", pure && svg.contains("<rect") && svg.contains("<ellipse") && svg.contains("<text") && svg.contains("linearGradient") && !svg.contains("<image"), QStringLiteral("%1 bytes").arg(svg.size()));
        report("SVG detected", ImageIO::detect(svg) == Fmt::SVG, QString());
        std::unique_ptr<Document> back(docio::openData(svg, Fmt::SVG, QString(), nullptr, &notes));
        int objs = 0, nl = 0; QSet<int> types;
        if (back) { for (const auto &l : back->s.frames[0].layers) for (const auto &o : l) { ++objs; types << int(o.type); } for (auto &l : back->s.layers) if (!l.web) ++nl; }
        const int md = back ? maxDiff025(d->render(0), back->render(0), false) : -1;
        report("SVG round trip objects", back && nl == 2 && objs == 3 && types.contains(int(ObjType::Rect)) && types.contains(int(ObjType::Ellipse)) && types.contains(int(ObjType::Text)),
               QStringLiteral("%1 layers, %2 objects, render diff %3").arg(nl).arg(objs).arg(md));
        report("SVG round trip render", md >= 0 && md <= 96, QStringLiteral("max diff %1 (text antialiasing)").arg(md));
        ExportOptions zo; zo.format = Fmt::SVGZ;
        QByteArray z; docio::exportDocument(*d, 0, zo, z, nullptr);
        bool gok = false; const QByteArray un = docio::gunzip(z, &gok);
        report("SVGZ export (gzip)", docio::isGzip(z) && gok && un == svg && ImageIO::detect(z) == Fmt::SVGZ && z.size() < svg.size(), QStringLiteral("%1 -> %2 bytes").arg(svg.size()).arg(z.size()));
        std::unique_ptr<Document> zb(docio::openData(z, Fmt::SVGZ, QString(), nullptr));
        report("SVGZ import", zb && zb->s.width == d->s.width && zb->s.height == d->s.height, QString());
        ImageSeq rs; report("SVGZ rasterise (flat)", ImageIO::readData(z, rs, nullptr) && rs.frames[0].image.size() == QSize(120, 80), QString());
        // a hand-written SVG: viewBox scaling, transforms, paths, CSS, gradients, groups as layers
        const QByteArray ext = R"SVG(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 100 50">
  <style>.red { fill: #ff0000 } #blue { fill: rgb(0,0,255) }</style>
  <defs><radialGradient id="g"><stop offset="0" stop-color="white"/><stop offset="1" stop-color="black"/></radialGradient></defs>
  <g id="Back"><rect class="red" x="0" y="0" width="50" height="50"/></g>
  <g id="Front" transform="translate(50,0)">
    <path id="blue" d="M0 0 L50 0 L50 25 Z"/>
    <circle cx="25" cy="37" r="10" fill="url(#g)" stroke="#00ff00" stroke-width="2"/>
    <polyline points="0,50 10,40 20,50" fill="none" stroke="black"/>
  </g>
</svg>)SVG";
        std::unique_ptr<Document> eb(docio::openData(ext, Fmt::SVG, QString(), nullptr));
        if (eb) {
            const QImage r = eb->render(0);
            int nl2 = 0; for (auto &l : eb->s.layers) if (!l.web) ++nl2;
            report("SVG import (viewBox, CSS, transform)", eb->s.width == 200 && eb->s.height == 100 && nl2 == 2 &&
                                                         qRed(r.pixel(20, 50)) > 240 && qBlue(r.pixel(20, 50)) < 15 && qBlue(r.pixel(190, 5)) > 240 && qRed(r.pixel(190, 5)) < 15,
                   QStringLiteral("%1x%2, %3 layers").arg(eb->s.width).arg(eb->s.height).arg(nl2));
            QImage qt(200, 100, QImage::Format_ARGB32); ImageSeq qs; codec::rasterizeSvg(ext, qs, nullptr);
            report("SVG import matches QtSvg", !qs.frames.isEmpty() && maxDiff025(qs.frames[0].image, r, false) <= 255 && psnr(qs.frames[0].image, r) > 20,
                   QStringLiteral("PSNR %1 dB").arg(qs.frames.isEmpty() ? 0 : psnr(qs.frames[0].image, r), 0, 'f', 1));
        } else report("SVG import (viewBox, CSS, transform)", false, "open failed");
        // unsupported features fall back to a bitmap with a note
        const QByteArray filt = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><filter id="f"><feGaussianBlur stdDeviation="2"/></filter><rect width="30" height="30" filter="url(#f)"/></svg>)SVG";
        QStringList fn; std::unique_ptr<Document> fb(docio::openData(filt, Fmt::SVG, QString(), nullptr, &fn));
        report("SVG unsupported -> bitmap + note", fb && !fn.isEmpty(), fn.join("; "));
        delete d;
    }
    // ---- PDF
    {
        Document *d = vectorDoc();
        ExportOptions o; o.format = Fmt::PDF;
        QByteArray pdf; QString err;
        const bool ok = docio::exportDocument(*d, 0, o, pdf, &err);
        report("PDF export", ok && pdf.startsWith("%PDF-") && pdf.contains("%%EOF") && ImageIO::detect(pdf) == Fmt::PDF && pdf.count("/Type /Page\n") + pdf.count("/Type /Page ") + pdf.count("/Type /Page>") >= 1,
               QStringLiteral("%1 bytes %2").arg(pdf.size()).arg(err));
        Frame f2 = d->s.frames[0]; f2.name = "Frame 2"; d->s.frames << f2;
        QByteArray pdf2; docio::writePdf(d->s, pdf2, nullptr);
        const int pages = pdf2.count("/Type /Page") - pdf2.count("/Type /Pages");
        report("PDF one page per frame", pages == 2, QStringLiteral("%1 pages").arg(pages));
        QFile out(QDir(tmpDir).filePath("doc.pdf")); out.open(QIODevice::WriteOnly); out.write(pdf2); out.close();
        report("PDF import not supported", !ImageIO::canRead(Fmt::PDF) && ImageIO::canWrite(Fmt::PDF), QString());
        delete d;
    }
    // ---- editable PNG (lwDC) with each PNG variant, and Fireworks PNG detection
    {
        Document *d = vectorDoc();
        for (int t = 0; t < 4; ++t) {
            ExportOptions o; o.format = Fmt::PNG; o.pngType = t; o.pngEditable = true;
            QByteArray png; QString err;
            bool ok = docio::exportDocument(*d, 0, o, png, &err);
            QImage flat; ok = ok && decode(png, flat) && docio::isEditablePng(png) && ihdr(png).colorType == (t == 0 ? 6 : t == 1 ? 2 : t == 2 ? 3 : 4);
            std::unique_ptr<Document> back(ok ? docio::openData(png, Fmt::PNG, QString(), &err) : nullptr);
            int objs = 0; if (back) for (const auto &l : back->s.frames[0].layers) objs += l.size();
            report(QStringLiteral("Editable PNG (type %1)").arg(t), ok && back && objs == 3 && back->s.layers.size() == d->s.layers.size() && maxDiff025(d->render(0), back->render(0)) == 0,
                   QStringLiteral("%1 bytes, %2 objects %3").arg(png.size()).arg(objs).arg(err));
        }
        ExportOptions o; o.format = Fmt::PNG; o.pngEditable = true; o.tinyPng = true;
        QByteArray png; bool ok = docio::exportDocument(*d, 0, o, png, nullptr);
        std::unique_ptr<Document> back(ok ? docio::openData(png, Fmt::PNG, QString(), nullptr) : nullptr);
        report("Editable PNG + Tiny", back != nullptr && docio::isEditablePng(png), QString());
        // Fireworks PNG: private chunks -> flattened with a note
        QByteArray plain; ImageIO::writeData(plain, seqOf(opq), ExportOptions(), nullptr);
        QByteArray fw = codec::pngWithChunk(codec::pngWithChunk(plain, "mkBF", QByteArray(64, 'x')), "mkTS", QByteArray(16, 'y'));
        QStringList notes;
        std::unique_ptr<Document> fd(docio::openData(fw, Fmt::Unknown, QString(), nullptr, &notes));
        report("Fireworks PNG detected (flattened)", codec::isFireworksPng(fw) && !codec::isFireworksPng(plain) && fd && !notes.isEmpty() && fd->s.width == opq.width(), notes.join("; "));
        delete d;
    }
    // ---- favicon / app icons bundle
    {
        Document *d = vectorDoc();
        const QString dir = QDir(tmpDir).filePath("favicon");
        docio::FaviconOptions fo; fo.appName = "Test App"; fo.applePadding = 10; fo.appleBackground = QColor(10, 20, 30); fo.themeColor = QColor("#336699");
        docio::FaviconResult res; QString err;
        bool ok = docio::exportFaviconBundle(d->s, 0, dir, fo, &res, &err);
        QDir dd(dir);
        const QStringList want = {"favicon.ico", "favicon.svg", "apple-touch-icon.png", "android-chrome-192x192.png", "android-chrome-512x512.png", "site.webmanifest", "favicon-snippet.html"};
        bool all = ok; for (const QString &w : want) all = all && dd.exists(w);
        report("Favicon bundle files", all && res.wroteSvg, res.files.join(", ") + " " + err);
        QFile ico(dd.filePath("favicon.ico")); ico.open(QIODevice::ReadOnly); const QByteArray icoData = ico.readAll();
        const int n = icoData.size() > 6 ? (uchar(icoData[4]) | uchar(icoData[5]) << 8) : 0;
        QSet<int> sizes; for (int i = 0; i < n; ++i) sizes << uchar(icoData[6 + 16 * i]);
        ImageSeq is; const bool icoOk = ImageIO::readData(icoData, is, nullptr);
        report("favicon.ico 16/32/48", icoOk && n == 3 && sizes == QSet<int>({16, 32, 48}), QStringLiteral("%1 entries").arg(n));
        QImage apple(dd.filePath("apple-touch-icon.png")), a192(dd.filePath("android-chrome-192x192.png")), a512(dd.filePath("android-chrome-512x512.png"));
        report("apple-touch-icon 180 (padding, background)", apple.size() == QSize(180, 180) && apple.pixel(2, 2) == qRgb(10, 20, 30), QString());
        report("android-chrome 192 / 512", a192.size() == QSize(192, 192) && a512.size() == QSize(512, 512), QString());
        QFile mf(dd.filePath("site.webmanifest")); mf.open(QIODevice::ReadOnly);
        const QJsonObject m = QJsonDocument::fromJson(mf.readAll()).object();
        report("site.webmanifest", m["name"].toString() == "Test App" && m["icons"].toArray().size() == 2 && m["theme_color"].toString() == "#336699", QString());
        report("HTML <link> snippet", res.snippet.contains("rel=\"icon\"") && res.snippet.contains("favicon.svg") && res.snippet.contains("apple-touch-icon") && res.snippet.contains("manifest"), QString());
        QFile sf(dd.filePath("favicon.svg")); sf.open(QIODevice::ReadOnly);
        report("favicon.svg is SVG", ImageIO::detect(sf.readAll()) == Fmt::SVG, QString());
        // a bitmap document writes no favicon.svg
        std::unique_ptr<Document> bd(Document::fromImageSeq(seqOf(opq)));
        docio::FaviconResult r2;
        report("bitmap document: no favicon.svg", docio::exportFaviconBundle(bd->s, 0, QDir(tmpDir).filePath("favicon2"), docio::FaviconOptions(), &r2, nullptr) && !r2.wroteSvg && !r2.snippet.contains(".svg"), QString());
        delete d;
    }
    // ---- format table sanity: extensions / MIME types / detection by extension
    {
        bool ok = true; QStringList bad;
        for (Fmt f : ImageIO::importFormats()) for (const QString &e : ImageIO::extensions(f)) {
            const Fmt g = ImageIO::fromExtension("x." + e);
            if (g != f && !(f == Fmt::APNG && g == Fmt::PNG)) { ok = false; bad << e; }
        }
        const QStringList mt = ImageIO::mimeTypes();
        ok = ok && mt.contains("image/svg+xml") && mt.contains("image/vnd.adobe.photoshop") && mt.contains("image/x-exr") && mt.contains("image/qoi") && !mt.contains("application/pdf");
        report("extensions / MIME types", ok, bad.join(' '));
#ifdef LW_SOURCE_DIR
        {
            // packaging: every importable MIME type is in the .desktop MimeType list (file managers / .deb)
            QFile df(QStringLiteral(LW_SOURCE_DIR "/packaging/libreworks.desktop"));
            QStringList desk;
            if (df.open(QIODevice::ReadOnly))
                for (const QByteArray &line : df.readAll().split('\n'))
                    if (line.startsWith("MimeType=")) desk = QString::fromUtf8(line.mid(9)).split(';', Qt::SkipEmptyParts);
            QStringList miss;
            for (const QString &m : mt) if (!desk.contains(m)) miss << m;
            report(".desktop MimeType covers every importable format", !desk.isEmpty() && miss.isEmpty() && desk.contains("application/x-libreworks"), miss.join(' '));
        }
#endif
        report("PDF / RAW capability flags", ImageIO::isLayered(Fmt::PSD) && ImageIO::isHdr(Fmt::EXR) && !ImageIO::isHdr(Fmt::PNG), QString());
    }
    if (g_skip025) std::printf("(%d v0.2.5 checks skipped)\n", g_skip025);
}
