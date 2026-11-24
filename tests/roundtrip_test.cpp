// Round-trip test: for each format, build a document, export, re-import and verify
// dimensions and frame count (plus a rough pixel check for lossless formats).
#include "core/Document.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "io/Codecs.h"
#include <functional>
#include <tiffio.h>
#include <QBuffer>
#include <QImageReader>
#include <QDir>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdio>

static int g_fail = 0, g_pass = 0, g_skip = 0;
void runFormatTests025(const QString &tmpDir, const std::function<void(const QString &, bool, const QString &)> &report);
void runCHeaderTests(const QString &tmpDir, const std::function<void(const QString &, bool, const QString &)> &report);

static void report(const QString &name, bool ok, const QString &detail = {}) {
    std::printf("%-34s %s %s\n", qPrintable(name), ok ? "PASS" : "FAIL", qPrintable(detail));
    (ok ? g_pass : g_fail)++;
}

static double meanDiff(const QImage &a, const QImage &b) {
    QImage x = a.convertToFormat(QImage::Format_ARGB32), y = b.convertToFormat(QImage::Format_ARGB32);
    double sum = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            sum += qAbs(qRed(p) - qRed(q)) + qAbs(qGreen(p) - qGreen(q)) + qAbs(qBlue(p) - qBlue(q));
        }
    return sum / (3.0 * x.width() * x.height());
}

static int maxDiff(const QImage &a, const QImage &b) {
    QImage x = a.convertToFormat(QImage::Format_ARGB32), y = b.convertToFormat(QImage::Format_ARGB32);
    int m = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            if (qAlpha(p) == 0 && qAlpha(q) == 0) continue;
            m = qMax(m, qAbs(qRed(p) - qRed(q)));
            m = qMax(m, qAbs(qGreen(p) - qGreen(q)));
            m = qMax(m, qAbs(qBlue(p) - qBlue(q)));
            m = qMax(m, qAbs(qAlpha(p) - qAlpha(q)));
        }
    return m;
}

struct Case { QString label; ExportOptions o; int frames; int tolerance; char netpbmMagic = 0; };

// Inspect a TIFF we wrote: compression, bits/sample, samples/pixel and page count.
struct TiffInfo { int comp = -1, bps = 0, spp = 0, pages = 0, extra = -1; };
static TiffInfo tiffInfo(const QString &path) {
    TiffInfo ti;
    TIFFSetWarningHandler(nullptr);
    TIFF *t = TIFFOpen(QFile::encodeName(path).constData(), "r");
    if (!t) return ti;
    uint16_t c = 0, b = 0, sp = 0, ne = 0; uint16_t *ex = nullptr;
    TIFFGetField(t, TIFFTAG_COMPRESSION, &c); TIFFGetField(t, TIFFTAG_BITSPERSAMPLE, &b); TIFFGetField(t, TIFFTAG_SAMPLESPERPIXEL, &sp);
    if (TIFFGetField(t, TIFFTAG_EXTRASAMPLES, &ne, &ex) && ne) ti.extra = ex[0];
    ti.comp = c; ti.bps = b; ti.spp = sp;
    do ++ti.pages; while (TIFFReadDirectory(t));
    TIFFClose(t);
    return ti;
}

// Hand-built TIFF via libtiff for import tests. 'samples' is row-major, contiguous, spp per pixel.
struct TiffSpec {
    int w = 4, h = 3, bps = 8, spp = 3, photo = PHOTOMETRIC_RGB, planar = PLANARCONFIG_CONTIG, extra = -1;
    int orient = ORIENTATION_TOPLEFT, comp = COMPRESSION_NONE; bool tiled = false;
    QVector<uint16_t> cmap; // palette: 3 * (1 << bps) entries (r..., g..., b...)
};
static QByteArray makeTiff(const QString &path, const TiffSpec &sp, const QVector<uint32_t> &samples) {
    TIFF *t = TIFFOpen(QFile::encodeName(path).constData(), "w");
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, uint32_t(sp.w)); TIFFSetField(t, TIFFTAG_IMAGELENGTH, uint32_t(sp.h));
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, uint16_t(sp.bps)); TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, uint16_t(sp.spp));
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, uint16_t(sp.photo)); TIFFSetField(t, TIFFTAG_PLANARCONFIG, uint16_t(sp.planar));
    TIFFSetField(t, TIFFTAG_ORIENTATION, uint16_t(sp.orient)); TIFFSetField(t, TIFFTAG_COMPRESSION, uint16_t(sp.comp));
    if (sp.extra >= 0) { uint16_t e[1] = {uint16_t(sp.extra)}; TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, e); }
    if (!sp.cmap.isEmpty()) { int n = 1 << sp.bps; TIFFSetField(t, TIFFTAG_COLORMAP, sp.cmap.constData(), sp.cmap.constData() + n, sp.cmap.constData() + 2 * n); }
    const int bytes = sp.bps / 8 ? sp.bps / 8 : 1;
    auto put = [&](QByteArray &buf, int idx, uint32_t v) {
        if (sp.bps == 16) { uint16_t x = uint16_t(v); memcpy(buf.data() + idx * 2, &x, 2); }
        else if (sp.bps == 8) buf[idx] = char(v);
        else if (v) buf[idx / 8] = char(buf[idx / 8] | (0x80 >> (idx % 8))); // 1-bit
    };
    if (sp.tiled) {
        TIFFSetField(t, TIFFTAG_TILEWIDTH, 16u); TIFFSetField(t, TIFFTAG_TILELENGTH, 16u);
        QByteArray tile(16 * 16 * sp.spp * bytes, 0);
        for (int y = 0; y < sp.h; ++y) for (int x = 0; x < sp.w; ++x) for (int c = 0; c < sp.spp; ++c)
            put(tile, (y * 16 + x) * sp.spp + c, samples[(y * sp.w + x) * sp.spp + c]);
        TIFFWriteTile(t, tile.data(), 0, 0, 0, 0);
    } else if (sp.planar == PLANARCONFIG_SEPARATE) {
        for (int c = 0; c < sp.spp; ++c) for (int y = 0; y < sp.h; ++y) {
            QByteArray line(sp.w * bytes, 0);
            for (int x = 0; x < sp.w; ++x) put(line, x, samples[(y * sp.w + x) * sp.spp + c]);
            TIFFWriteScanline(t, line.data(), uint32_t(y), uint16_t(c));
        }
    } else {
        for (int y = 0; y < sp.h; ++y) {
            QByteArray line((sp.w * sp.spp * sp.bps + 7) / 8, 0);
            for (int x = 0; x < sp.w; ++x) for (int c = 0; c < sp.spp; ++c) put(line, x * sp.spp + c, samples[(y * sp.w + x) * sp.spp + c]);
            TIFFWriteScanline(t, line.data(), uint32_t(y), 0);
        }
    }
    TIFFClose(t);
    QFile f(path); f.open(QIODevice::ReadOnly);
    return f.readAll();
}

// Hand-built TGA: header + optional colour map + pixel bytes (already encoded) + optional footer.
static QByteArray makeTga(int type, int w, int h, int bpp, int desc, const QByteArray &pixels,
                          const QByteArray &cmap = {}, int cmLen = 0, int cmBits = 0, int cmFirst = 0) {
    QByteArray d(18, '\0');
    d[1] = char(cmLen ? 1 : 0); d[2] = char(type);
    d[3] = char(cmFirst & 0xFF); d[4] = char(cmFirst >> 8); d[5] = char(cmLen & 0xFF); d[6] = char(cmLen >> 8); d[7] = char(cmBits);
    d[12] = char(w); d[13] = char(w >> 8); d[14] = char(h); d[15] = char(h >> 8); d[16] = char(bpp); d[17] = char(desc);
    return d + cmap + pixels;
}

static bool imagesEqual(const QImage &a, const QImage &b) { return a.size() == b.size() && maxDiff(a, b) == 0; }

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QTemporaryDir tmp;
    if (qEnvironmentVariableIsSet("LW_KEEP_TMP")) { tmp.setAutoRemove(false); std::printf("Temp dir: %s\n", qPrintable(tmp.path())); }
    Document *doc = Document::createSample();
    const int W = doc->s.width, H = doc->s.height;
    const int NF = doc->s.frames.size();
    std::printf("Sample document: %dx%d, %d frames, %d layers\n", W, H, NF, int(doc->s.layers.size()));
    std::printf("Codecs: %s\n\n", qPrintable(ImageIO::codecSummary()));

    QVector<Case> cases;
    auto add = [&](const QString &label, Fmt f, bool anim, int tol, std::function<void(ExportOptions &)> tweak = {}) {
        ExportOptions o; o.format = f; o.animated = anim;
        if (tweak) tweak(o);
        cases.append({label, o, anim ? NF : 1, tol});
        return &cases.last();
    };
    add("PNG", Fmt::PNG, false, 0);
    add("APNG (static)", Fmt::APNG, false, 0);
    add("APNG (animated)", Fmt::APNG, true, 0);
    add("JPEG q90", Fmt::JPEG, false, 80, [](ExportOptions &o) { o.quality = 90; });
    add("WebP lossy q80", Fmt::WEBP, false, 120);
    add("WebP lossless", Fmt::WEBP, false, 0, [](ExportOptions &o) { o.lossless = true; });
    add("WebP animated lossy", Fmt::WEBP, true, 120);
    add("WebP animated lossless", Fmt::WEBP, true, 0, [](ExportOptions &o) { o.lossless = true; });
    add("GIF 256 dither", Fmt::GIF, false, -1);
    add("GIF 32 web216", Fmt::GIF, false, -1, [](ExportOptions &o) { o.colors = 32; o.palette = 1; o.dither = false; });
    add("GIF animated", Fmt::GIF, true, -1);
    add("AVIF q70", Fmt::AVIF, false, 140, [](ExportOptions &o) { o.quality = 70; });
    add("AVIF lossless", Fmt::AVIF, false, 2, [](ExportOptions &o) { o.lossless = true; });
    add("AVIF animated", Fmt::AVIF, true, 140);
    add("JXL q90", Fmt::JXL, false, 80, [](ExportOptions &o) { o.quality = 90; });
    add("JXL lossless", Fmt::JXL, false, 0, [](ExportOptions &o) { o.lossless = true; });
    add("JXL animated", Fmt::JXL, true, 80);
    add("BMP", Fmt::BMP, false, -1);
    add("TIFF (default LZW)", Fmt::TIFF, false, 0);
    add("TIFF none", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiffCompression = 0; });
    add("TIFF LZW", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiffCompression = 1; });
    add("TIFF Deflate", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiffCompression = 2; });
    add("TIFF PackBits", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiffCompression = 3; });
    add("TIFF 16-bit none", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiff16 = true; o.tiffCompression = 0; });
    add("TIFF 16-bit LZW", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiff16 = true; o.tiffCompression = 1; });
    add("TIFF 16-bit Deflate", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiff16 = true; o.tiffCompression = 2; });
    add("TIFF 16-bit PackBits", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.tiff16 = true; o.tiffCompression = 3; });
    add("TIFF no alpha (RGB)", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.alpha = false; });
    add("TIFF 16-bit no alpha", Fmt::TIFF, false, 0, [](ExportOptions &o) { o.alpha = false; o.tiff16 = true; });
    add("TIFF multi-page LZW", Fmt::TIFF, true, 0);
    add("TIFF multi-page 16-bit Deflate", Fmt::TIFF, true, 0, [](ExportOptions &o) { o.tiff16 = true; o.tiffCompression = 2; });
    add("TGA 32-bit RLE", Fmt::TGA, false, 0);
    add("TGA 32-bit raw", Fmt::TGA, false, 0, [](ExportOptions &o) { o.tgaRle = false; });
    add("TGA 24-bit RLE", Fmt::TGA, false, 0, [](ExportOptions &o) { o.alpha = false; });
    add("TGA 24-bit raw", Fmt::TGA, false, 0, [](ExportOptions &o) { o.alpha = false; o.tgaRle = false; });
    add("TGA 32-bit RLE top-left", Fmt::TGA, false, 0, [](ExportOptions &o) { o.tgaTopLeft = true; });
    add("TGA 32-bit raw top-left", Fmt::TGA, false, 0, [](ExportOptions &o) { o.tgaTopLeft = true; o.tgaRle = false; });
    add("TGA 24-bit RLE top-left", Fmt::TGA, false, 0, [](ExportOptions &o) { o.alpha = false; o.tgaTopLeft = true; });
    add("TGA 24-bit raw top-left", Fmt::TGA, false, 0, [](ExportOptions &o) { o.alpha = false; o.tgaRle = false; o.tgaTopLeft = true; });
    add("PPM binary (P6)", Fmt::PPM, false, -1)->netpbmMagic = '6';
    add("PPM ASCII (P3)", Fmt::PPM, false, -1, [](ExportOptions &o) { o.netpbmAscii = true; })->netpbmMagic = '3';
    add("PGM binary (P5)", Fmt::PGM, false, -1)->netpbmMagic = '5';
    add("PGM ASCII (P2)", Fmt::PGM, false, -1, [](ExportOptions &o) { o.netpbmAscii = true; })->netpbmMagic = '2';

    int caseNo = 0;
    for (const Case &c : cases) {
        ++caseNo;
        if (!ImageIO::available(c.o.format)) { std::printf("%-34s SKIP (codec not built)\n", qPrintable(c.label)); g_skip++; continue; }
        ImageSeq seq = doc->toImageSeq(c.o.animated);
        QString path = tmp.filePath(QStringLiteral("rt_%1.%2").arg(caseNo).arg(ImageIO::extension(c.o.format)));
        QString err;
        if (!ImageIO::write(path, seq, c.o, &err)) { report(c.label, false, "export: " + err); continue; }
        QFile f(path); f.open(QIODevice::ReadOnly);
        QByteArray bytes = f.readAll();
        ImageSeq back;
        if (!ImageIO::read(path, back, &err)) { report(c.label, false, "import: " + err); continue; }
        bool ok = !back.frames.isEmpty() && back.frames[0].image.width() == W && back.frames[0].image.height() == H && back.frames.size() == c.frames;
        QString detail = QStringLiteral("%1 bytes, %2x%3, %4 frame(s), detected %5")
                             .arg(bytes.size()).arg(back.frames.isEmpty() ? 0 : back.frames[0].image.width())
                             .arg(back.frames.isEmpty() ? 0 : back.frames[0].image.height()).arg(back.frames.size())
                             .arg(ImageIO::formatName(ImageIO::detect(bytes)));
        if ((c.o.format == Fmt::TIFF || c.o.format == Fmt::TGA) && ImageIO::detect(bytes) != c.o.format) { ok = false; detail += " (not detected)"; }
        if (c.o.format == Fmt::TIFF) {
            static const int comps[] = {COMPRESSION_NONE, COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE, COMPRESSION_PACKBITS};
            TiffInfo ti = tiffInfo(path);
            detail += QStringLiteral(", tiff comp %1 bps %2 spp %3 pages %4").arg(ti.comp).arg(ti.bps).arg(ti.spp).arg(ti.pages);
            if (ti.comp != comps[c.o.tiffCompression] || ti.bps != (c.o.tiff16 ? 16 : 8) || ti.spp != (c.o.alpha ? 4 : 3) || ti.pages != c.frames ||
                (c.o.alpha && ti.extra != EXTRASAMPLE_UNASSALPHA)) ok = false;
        }
        if (c.o.format == Fmt::TGA && bytes.size() > 18) {
            const int type = uchar(bytes[2]), bpp = uchar(bytes[16]), desc = uchar(bytes[17]);
            detail += QStringLiteral(", tga type %1 bpp %2 desc 0x%3").arg(type).arg(bpp).arg(desc, 2, 16, QChar('0'));
            if (type != (c.o.tgaRle ? 10 : 2) || bpp != (c.o.alpha ? 32 : 24) || bool(desc & 0x20) != c.o.tgaTopLeft || (desc & 15) != (c.o.alpha ? 8 : 0)) ok = false;
        }
        // Interop: Qt's own TIFF / TGA plugins (when installed) must read our files identically.
        if (ok && (c.o.format == Fmt::TIFF || c.o.format == Fmt::TGA)) {
            const QByteArray qfmt = c.o.format == Fmt::TIFF ? "tiff" : "tga";
            // (Qt's TGA plugin reads uncompressed TGA only; its TIFF plugin is skipped for 16-bit.)
            if (QImageReader::supportedImageFormats().contains(qfmt) && !(c.o.format == Fmt::TIFF && c.o.tiff16) && !(c.o.format == Fmt::TGA && c.o.tgaRle)) {
                QImageReader r(path, qfmt);
                QImage qi = r.read();
                int d = qi.isNull() ? -1 : maxDiff(qi, back.frames[0].image);
                detail += QStringLiteral(", Qt plugin diff %1").arg(d);
                if (d != 0) ok = false;
            }
        }
        if (c.netpbmMagic && (bytes.size() < 2 || bytes[1] != c.netpbmMagic)) { ok = false; detail += QStringLiteral(" wrong magic P%1").arg(QChar(bytes[1])); }
        if (ok && c.tolerance >= 0) {
            QImage ref = seq.frames.last().image;
            if (!ImageIO::hasAlpha(c.o)) ref = codec::flatten(ref, c.o.matte);
            int d = maxDiff(ref, back.frames.last().image);
            double md = meanDiff(ref, back.frames.last().image);
            detail += QStringLiteral(", diff max %1 mean %2").arg(d).arg(md, 0, 'f', 2);
            // Lossless formats must be exact; lossy ones must stay close on average.
            if (c.tolerance == 0 ? d != 0 : md > 4.0) ok = false;
        }
        if (ok && c.frames > 1) {
            int delay = back.frames[0].delayMs;
            detail += QStringLiteral(", delay %1ms").arg(delay);
            if (qAbs(delay - doc->s.frames[0].delayMs) > 10) ok = false;
        }
        report(c.label, ok, detail);
    }

    // Hand-written PBM (P1 ASCII and P4 binary) import, plus 16-bit PGM.
    {
        QByteArray p1 = "P1\n# tiny\n5 3\n10101\n0 1 0 1 0\n11111\n";
        ImageSeq s; QString err;
        bool ok = ImageIO::readData(p1, s, &err) && s.frames.size() == 1 && s.frames[0].image.size() == QSize(5, 3) &&
                  qGray(s.frames[0].image.pixel(0, 0)) == 0 && qGray(s.frames[0].image.pixel(1, 0)) == 255;
        report("PBM ASCII (P1) import", ok, err);
        QByteArray p4 = "P4\n10 2\n";
        p4.append(char(0xA0)); p4.append(char(0x40)); p4.append(char(0xFF)); p4.append(char(0x80));
        s = {};
        ok = ImageIO::readData(p4, s, &err) && s.frames[0].image.size() == QSize(10, 2) &&
             qGray(s.frames[0].image.pixel(0, 0)) == 0 && qGray(s.frames[0].image.pixel(1, 0)) == 255 &&
             qGray(s.frames[0].image.pixel(9, 0)) == 0 && qGray(s.frames[0].image.pixel(9, 1)) == 255;
        report("PBM binary (P4) import", ok, err);
        QByteArray p5 = "P5 2 1 65535\n";
        p5.append(char(0xFF)); p5.append(char(0xFF)); p5.append(char(0x80)); p5.append(char(0x00));
        s = {};
        ok = ImageIO::readData(p5, s, &err) && s.frames[0].image.size() == QSize(2, 1) &&
             qGray(s.frames[0].image.pixel(0, 0)) == 255 && qAbs(qGray(s.frames[0].image.pixel(1, 0)) - 128) <= 1;
        report("PGM 16-bit (P5) import", ok, err);
    }

    // ---- TIFF import variants (files built directly with libtiff).
    {
        auto check = [&](const QString &label, const TiffSpec &sp, const QVector<uint32_t> &samples, const QVector<QRgb> &expect) {
            QString path = tmp.filePath(label + ".tif"), err;
            makeTiff(path, sp, samples);
            ImageSeq s;
            bool ok = ImageIO::read(path, s, &err) && s.frames.size() == 1;
            QString detail = err;
            if (ok) {
                const QImage &im = s.frames[0].image;
                ok = im.width() == (sp.orient >= 5 ? sp.h : sp.w) && im.height() == (sp.orient >= 5 ? sp.w : sp.h);
                for (int i = 0; ok && i < expect.size(); ++i) {
                    QRgb got = im.pixel(i % im.width(), i / im.width());
                    bool close = qAbs(qRed(got) - qRed(expect[i])) <= 1 && qAbs(qGreen(got) - qGreen(expect[i])) <= 1 &&
                                 qAbs(qBlue(got) - qBlue(expect[i])) <= 1 && qAbs(qAlpha(got) - qAlpha(expect[i])) <= 1;
                    if (!close) { ok = false; detail = QStringLiteral("pixel %1: got %2 want %3").arg(i).arg(got, 8, 16, QChar('0')).arg(expect[i], 8, 16, QChar('0')); }
                }
            }
            report("TIFF import: " + label, ok, detail);
        };
        TiffSpec g; g.spp = 1; g.photo = PHOTOMETRIC_MINISBLACK; g.w = 3; g.h = 1;
        check("grey 8-bit", g, {0, 128, 255}, {qRgb(0, 0, 0), qRgb(128, 128, 128), qRgb(255, 255, 255)});
        TiffSpec gw = g; gw.photo = PHOTOMETRIC_MINISWHITE;
        check("grey min-is-white", gw, {0, 128, 255}, {qRgb(255, 255, 255), qRgb(127, 127, 127), qRgb(0, 0, 0)});
        TiffSpec g16 = g; g16.bps = 16; g16.spp = 2; g16.extra = EXTRASAMPLE_UNASSALPHA;
        check("grey+alpha 16-bit", g16, {0, 65535, 32896, 65535, 65535, 0}, {qRgba(0, 0, 0, 255), qRgba(128, 128, 128, 255), qRgba(255, 255, 255, 0)});
        TiffSpec r16; r16.bps = 16; r16.w = 2; r16.h = 1;
        check("RGB 16-bit", r16, {65535, 0, 0, 257 * 10, 257 * 20, 257 * 30}, {qRgb(255, 0, 0), qRgb(10, 20, 30)});
        TiffSpec ua; ua.spp = 4; ua.extra = EXTRASAMPLE_UNASSALPHA; ua.w = 2; ua.h = 1;
        check("RGBA unassociated", ua, {200, 100, 50, 128, 10, 20, 30, 255}, {qRgba(200, 100, 50, 128), qRgba(10, 20, 30, 255)});
        TiffSpec aa = ua; aa.extra = EXTRASAMPLE_ASSOCALPHA;
        check("RGBA associated", aa, {100, 50, 25, 128, 0, 0, 0, 0}, {qRgba(199, 100, 50, 128), qRgba(0, 0, 0, 0)});
        TiffSpec aa16 = aa; aa16.bps = 16;
        check("RGBA associated 16-bit", aa16, {32768, 16384, 0, 32768, 65535, 65535, 65535, 65535}, {qRgba(255, 128, 0, 128), qRgba(255, 255, 255, 255)});
        TiffSpec un = ua; un.extra = EXTRASAMPLE_UNSPECIFIED;
        check("RGBA unspecified extra", un, {200, 100, 50, 128, 10, 20, 30, 255}, {qRgba(200, 100, 50, 128), qRgba(10, 20, 30, 255)});
        TiffSpec pal; pal.spp = 1; pal.photo = PHOTOMETRIC_PALETTE; pal.w = 3; pal.h = 1;
        pal.cmap.fill(0, 3 * 256);
        pal.cmap[1] = 65535; pal.cmap[256 + 2] = 65535; pal.cmap[512 + 3] = 65535; pal.cmap[4] = 257 * 40;
        check("palette 8-bit", pal, {1, 2, 3}, {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255)});
        TiffSpec sep; sep.planar = PLANARCONFIG_SEPARATE; sep.w = 2; sep.h = 2; sep.comp = COMPRESSION_LZW;
        check("RGB planar separate LZW", sep, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, {qRgb(1, 2, 3), qRgb(4, 5, 6), qRgb(7, 8, 9), qRgb(10, 11, 12)});
        TiffSpec tl; tl.tiled = true; tl.w = 2; tl.h = 1; tl.spp = 4; tl.extra = EXTRASAMPLE_UNASSALPHA;
        check("RGBA tiled (libtiff RGBA path)", tl, {255, 0, 0, 255, 0, 0, 255, 255}, {qRgb(255, 0, 0), qRgb(0, 0, 255)});
        TiffSpec bw; bw.bps = 1; bw.spp = 1; bw.photo = PHOTOMETRIC_MINISBLACK; bw.w = 3; bw.h = 1;
        check("bilevel 1-bit", bw, {1, 0, 1}, {qRgb(255, 255, 255), qRgb(0, 0, 0), qRgb(255, 255, 255)});
        TiffSpec bl; bl.w = 2; bl.h = 2; bl.orient = ORIENTATION_BOTLEFT;
        check("orientation bottom-left", bl, {1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4}, {qRgb(3, 3, 3), qRgb(4, 4, 4), qRgb(1, 1, 1), qRgb(2, 2, 2)});
        TiffSpec rt; rt.w = 2; rt.h = 1; rt.orient = ORIENTATION_RIGHTTOP;
        check("orientation right-top (90 cw)", rt, {1, 1, 1, 2, 2, 2}, {qRgb(1, 1, 1), qRgb(2, 2, 2)});
        // Multi-page import keeps pages as frames; garbage is rejected cleanly.
        ImageSeq s; QString err;
        bool bad = !ImageIO::readData(QByteArray("II*\0garbage-garbage-garbage", 28), s, &err) && !err.isEmpty();
        report("TIFF import: corrupt file rejected", bad, err);
    }

    // ---- TGA import variants (hand-built byte streams).
    {
        auto check = [&](const QString &label, const QByteArray &d, int w, int h, const QVector<QRgb> &expect) {
            ImageSeq s; QString err;
            bool ok = ImageIO::detect(d) == Fmt::TGA && ImageIO::readData(d, s, &err) && s.frames.size() == 1 && s.frames[0].image.size() == QSize(w, h);
            QString detail = ImageIO::detect(d) == Fmt::TGA ? err : QStringLiteral("not detected as TGA");
            for (int i = 0; ok && i < expect.size(); ++i) {
                QRgb got = s.frames[0].image.pixel(i % w, i / w);
                if (got != expect[i]) { ok = false; detail = QStringLiteral("pixel %1: got %2 want %3").arg(i).arg(got, 8, 16, QChar('0')).arg(expect[i], 8, 16, QChar('0')); }
            }
            report("TGA import: " + label, ok, detail);
        };
        auto bytes = [](std::initializer_list<int> l) { QByteArray b; for (int v : l) b.append(char(v)); return b; };
        // 2x2 bottom-up: stream rows are bottom first.
        check("grey 8-bit raw bottom-up", makeTga(3, 2, 2, 8, 0x00, bytes({30, 40, 10, 20})), 2, 2, {qRgb(10, 10, 10), qRgb(20, 20, 20), qRgb(30, 30, 30), qRgb(40, 40, 40)});
        check("grey 8-bit RLE top-down", makeTga(11, 3, 1, 8, 0x20, bytes({0x82, 77})), 3, 1, {qRgb(77, 77, 77), qRgb(77, 77, 77), qRgb(77, 77, 77)});
        check("grey+alpha 16-bit", makeTga(3, 1, 1, 16, 0x28, bytes({90, 128})), 1, 1, {qRgba(90, 90, 90, 128)});
        check("24-bit raw top-down", makeTga(2, 2, 1, 24, 0x20, bytes({3, 2, 1, 6, 5, 4})), 2, 1, {qRgb(1, 2, 3), qRgb(4, 5, 6)});
        check("24-bit right-to-left", makeTga(2, 2, 1, 24, 0x30, bytes({3, 2, 1, 6, 5, 4})), 2, 1, {qRgb(4, 5, 6), qRgb(1, 2, 3)});
        // RLE packet crossing a scan line (allowed by many writers)
        check("24-bit RLE across lines", makeTga(10, 2, 2, 24, 0x20, bytes({0x82, 0, 0, 255, 0x00, 0, 255, 0})), 2, 2, {qRgb(255, 0, 0), qRgb(255, 0, 0), qRgb(255, 0, 0), qRgb(0, 255, 0)});
        check("32-bit alpha bits 8", makeTga(2, 1, 1, 32, 0x28, bytes({30, 20, 10, 64})), 1, 1, {qRgba(10, 20, 30, 64)});
        check("32-bit empty alpha -> opaque", makeTga(2, 2, 1, 32, 0x20, bytes({30, 20, 10, 0, 3, 2, 1, 0})), 2, 1, {qRgb(10, 20, 30), qRgb(1, 2, 3)});
        // 16-bit 1555: A=1 R=31 G=0 B=0 -> 0xFC00 ; A=0 green -> 0x03E0
        check("16-bit with alpha bit", makeTga(2, 2, 1, 16, 0x21, bytes({0x00, 0xFC, 0xE0, 0x03})), 2, 1, {qRgba(255, 0, 0, 255), qRgba(0, 255, 0, 0)});
        check("15-bit (no alpha)", makeTga(2, 2, 1, 15, 0x20, bytes({0x1F, 0x00, 0xE0, 0x03})), 2, 1, {qRgb(0, 0, 255), qRgb(0, 255, 0)});
        check("16-bit RLE", makeTga(10, 2, 1, 16, 0x21, bytes({0x81, 0x00, 0xFC})), 2, 1, {qRgba(255, 0, 0, 255), qRgba(255, 0, 0, 255)});
        // colour-mapped: 3-entry 24-bit map starting at index 5
        QByteArray map24 = bytes({0, 0, 255, 0, 255, 0, 255, 0, 0});
        check("colour-mapped 8-bit raw", makeTga(1, 3, 1, 8, 0x20, bytes({5, 6, 7}), map24, 3, 24, 5), 3, 1, {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgb(0, 0, 255)});
        check("colour-mapped 8-bit RLE bottom-up", makeTga(9, 1, 2, 8, 0x00, bytes({0x00, 7, 0x00, 5}), map24, 3, 24, 5), 1, 2, {qRgb(255, 0, 0), qRgb(0, 0, 255)});
        QByteArray map32 = bytes({0, 0, 255, 100, 255, 255, 255, 255});
        check("colour-mapped 32-bit map", makeTga(1, 2, 1, 8, 0x28, bytes({0, 1}), map32, 2, 32), 2, 1, {qRgba(255, 0, 0, 100), qRgba(255, 255, 255, 255)});
        QByteArray map16 = bytes({0x00, 0xFC, 0x1F, 0x00});
        check("colour-mapped 16-bit map", makeTga(1, 2, 1, 8, 0x21, bytes({0, 1}), map16, 2, 16), 2, 1, {qRgba(255, 0, 0, 255), qRgba(0, 0, 255, 0)});
        // TGA 2.0 extension area with attributes type 0 (no alpha) overrides the pixel alpha.
        {
            QByteArray d = makeTga(2, 1, 1, 32, 0x28, bytes({30, 20, 10, 64}));
            quint32 off = d.size();
            QByteArray ext(495, '\0'); ext[0] = char(495 & 0xFF); ext[1] = char(495 >> 8); ext[494] = 0;
            d += ext;
            d.append(char(off)); d.append(char(off >> 8)); d.append(char(0)); d.append(char(0)); d.append(QByteArray(4, '\0'));
            d.append("TRUEVISION-XFILE.", 17); d.append('\0');
            check("2.0 ext area: attributes no-alpha", d, 1, 1, {qRgb(10, 20, 30)});
            d[off + 494] = 4; // premultiplied
            check("2.0 ext area: premultiplied", d, 1, 1, {qRgba(40, 80, 120, 64)});
        }
        ImageSeq s; QString err;
        report("TGA import: truncated rejected", !ImageIO::readData(makeTga(2, 4, 4, 24, 0, bytes({1, 2, 3})) , s, &err), err);
        // Cross-check with Qt's TGA plugin on a file Qt can read.
        if (QImageReader::supportedImageFormats().contains("tga")) {
            QByteArray d = makeTga(2, 2, 2, 32, 0x08, bytes({1, 2, 3, 200, 1, 2, 3, 200, 9, 8, 7, 255, 6, 5, 4, 128}));
            d.append(QByteArray(8, '\0')); d.append("TRUEVISION-XFILE.", 17); d.append('\0'); // Qt's plugin needs the 2.0 footer
            QBuffer b(&d); b.open(QIODevice::ReadOnly);
            QImage qi = QImageReader(&b, "tga").read();
            ImageSeq ours; err.clear(); ImageIO::readData(d, ours, &err);
            report("TGA import: matches Qt plugin", !qi.isNull() && !ours.frames.isEmpty() && imagesEqual(qi, ours.frames[0].image), err);
        }
    }

    // ---- TIFF multi-page import: pages of different sizes become same-size frames.
    {
        ImageSeq seq;
        QImage a(8, 6, QImage::Format_ARGB32); a.fill(Qt::red);
        QImage b(8, 6, QImage::Format_ARGB32); b.fill(QColor(0, 0, 255, 128));
        QImage c(8, 6, QImage::Format_ARGB32); c.fill(Qt::green);
        seq.frames = {{a, 100}, {b, 100}, {c, 100}}; seq.width = 8; seq.height = 6;
        ExportOptions o; o.format = Fmt::TIFF; o.animated = true; o.tiffCompression = 3;
        QByteArray out; QString err;
        bool ok = ImageIO::writeData(out, seq, o, &err);
        ImageSeq back;
        ok = ok && ImageIO::readData(out, back, &err) && back.frames.size() == 3 && imagesEqual(back.frames[1].image, b) && imagesEqual(back.frames[2].image, c);
        Document *imp = ok ? Document::fromImageSeq(back) : nullptr;
        report("TIFF multi-page -> 3 frames", ok && imp && imp->s.frames.size() == 3, err);
        delete imp;
        // 16-bit export keeps full 8-bit precision (v * 257) and alpha.
        QImage grad(256, 1, QImage::Format_ARGB32);
        for (int x = 0; x < 256; ++x) grad.setPixel(x, 0, qRgba(x, 255 - x, x / 2, 255 - x));
        seq.frames = {{grad, 100}}; seq.width = 256; seq.height = 1;
        o.animated = false; o.tiff16 = true;
        ok = ImageIO::writeData(out, seq, o, &err) && ImageIO::readData(out, back, &err) && back.frames.size() == 1;
        report("TIFF 16-bit alpha gradient exact", ok && imagesEqual(back.frames[0].image, grad), err);
        // TGA with every alpha level
        o = ExportOptions(); o.format = Fmt::TGA;
        ok = ImageIO::writeData(out, seq, o, &err) && ImageIO::readData(out, back, &err);
        report("TGA 32-bit alpha gradient exact", ok && imagesEqual(back.frames[0].image, grad), err);
    }

    // CMYK and 32-bit float TIFFs written directly with libtiff.
    {
        QString path = tmp.filePath("cmyk.tif"), err;
        TIFF *t = TIFFOpen(QFile::encodeName(path).constData(), "w");
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, 2u); TIFFSetField(t, TIFFTAG_IMAGELENGTH, 1u);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 8); TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED); TIFFSetField(t, TIFFTAG_INKSET, INKSET_CMYK);
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        uint8_t row[8] = {255, 0, 0, 0, /* cyan */ 0, 0, 0, 255 /* black */};
        TIFFWriteScanline(t, row, 0, 0); TIFFClose(t);
        ImageSeq back;
        bool ok = ImageIO::read(path, back, &err) && back.frames.size() == 1;
        ok = ok && back.frames[0].image.pixel(0, 0) == qRgba(0, 255, 255, 255) && back.frames[0].image.pixel(1, 0) == qRgba(0, 0, 0, 255);
        report("TIFF import: CMYK", ok, err);

        path = tmp.filePath("float.tif");
        t = TIFFOpen(QFile::encodeName(path).constData(), "w");
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, 3u); TIFFSetField(t, TIFFTAG_IMAGELENGTH, 1u);
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 32); TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK); TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        float frow[3] = {0.0f, 0.5f, 2.0f};
        TIFFWriteScanline(t, frow, 0, 0); TIFFClose(t);
        ok = ImageIO::read(path, back, &err) && back.frames.size() == 1;
        ok = ok && qRed(back.frames[0].image.pixel(0, 0)) == 0 && qRed(back.frames[0].image.pixel(1, 0)) == 128 && qRed(back.frames[0].image.pixel(2, 0)) == 255;
        report("TIFF import: 32-bit float grey (clamped)", ok, err);
    }

    // Native document round-trip.
    {
        QString path = tmp.filePath("sample.lwd"), err;
        bool ok = lwd::save(*doc, path, &err);
        Document *back = ok ? lwd::load(path, &err) : nullptr;
        ok = back && back->s.width == W && back->s.height == H && back->s.frames.size() == NF &&
             back->s.layers.size() == doc->s.layers.size();
        int d = ok ? maxDiff(doc->render(1), back->render(1)) : -1;
        if (d != 0) ok = false;
        int objs = 0;
        if (back) for (auto &l : back->s.frames[0].layers) objs += l.size();
        report("LWD native document", ok, QStringLiteral("%1 layers, %2 frames, %3 objects in frame 1, render diff %4 %5")
                   .arg(back ? back->s.layers.size() : 0).arg(back ? back->s.frames.size() : 0).arg(objs).arg(d).arg(err));
        // Import an animation as frames.
        ImageSeq seq = doc->toImageSeq(true);
        Document *imp = Document::fromImageSeq(seq);
        report("Import animation as frames", imp->s.frames.size() == NF && imp->s.width == W, QStringLiteral("%1 frames").arg(imp->s.frames.size()));
        delete back; delete imp;
    }

    runFormatTests025(tmp.path(), [](const QString &n, bool ok, const QString &d) { report(n, ok, d); });
    runCHeaderTests(tmp.path(), [](const QString &n, bool ok, const QString &d) { report(n, ok, d); });

    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    delete doc;
    return g_fail ? 1 : 0;
}
