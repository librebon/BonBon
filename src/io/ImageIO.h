#pragma once
#include "Brand.h"
#include <QColor>
#include <QImage>
#include <QList>
#include <QString>
#include <QVector>

enum class Fmt {
    PNG, APNG, JPEG, WEBP, GIF, AVIF, JXL, BMP, TIFF, TGA, PPM, PGM, PBM,
    // v0.2.5
    PSD, ORA, XCF, SVG, PDF, HEIC, RAW, EXR, HDR, DDS, KTX2, QOI, ICO, CUR, ICNS, PAM, PFM, PCX, XPM, XBM, SVGZ,
    // v0.2.6
    CHeader,
    Unknown
};

// v0.2.7: the remembered default of "strip spaces/tabs/newlines from arrays" (QSettings export/chStrip, set at startup)
bool cheader_stripDefault();
void cheader_setStripDefault(bool on);

struct ExportOptions {
    Fmt format = Fmt::PNG;
    int quality = 80;           // JPEG / WebP / AVIF / JXL, 0..100
    bool lossless = false;      // WebP / AVIF / JXL
    int webpEffort = 3;         // WebP: libwebp method 0..6; higher is smaller and slower
    int webpAlphaQuality = 100; // WebP: alpha quality 0..100
    bool webpSharpYuv = false;  // WebP: sharp YUV downsampling
    bool webpExact = false;     // WebP lossless: keep RGB of transparent pixels
    int avifSpeed = 8;          // AVIF: 0 (slowest) .. 10 (fastest)
    int avifChroma = 2;         // AVIF: 0 4:4:4, 1 4:2:2, 2 4:2:0; lossless stays 4:4:4
    int avifDepth = 8;          // AVIF: 8, 10 or 12
    int avifAlphaQuality = -1;  // AVIF: -1 = quality+10, else 0..100
    int jxlEffort = 5;          // JPEG XL: 1 (fastest) .. 9 (slowest / smaller)
    QString heifPreset;         // HEIF: x265 preset; empty = encoder default (current behaviour)
    int heifChroma = -1;        // HEIF: -1 = encoder default; 0 4:2:0, 1 4:2:2, 2 4:4:4
    int colors = 256;           // GIF palette size 2..256
    int palette = 0;            // 0 adaptive, 1 web 216, 2 grayscale, 3 black & white
    bool dither = true;         // GIF
    int ditherAmount = 100;     // GIF dither strength 0..100 % (when dither is on)
    int loss = 0;               // GIF lossy compression 0..100 (longer runs for LZW, Fireworks "Loss")
    bool animated = false;      // export all frames (GIF/WebP/APNG/AVIF/JXL)
    bool netpbmAscii = false;   // PGM/PPM: false = binary P5/P6, true = ASCII P2/P3
    int tiffCompression = 1;    // TIFF: 0 none, 1 LZW, 2 Deflate (ZIP), 3 PackBits
    bool tiff16 = false;        // TIFF: 16 bits per channel instead of 8
    bool alpha = true;          // TIFF / TGA: write an alpha channel (TGA: 32-bit instead of 24-bit)
    bool tgaRle = true;         // TGA: RLE compression
    bool tgaTopLeft = false;    // TGA: top-left origin (false = standard bottom-left)
    // v0.2.5
    int pngType = 0;            // PNG: 0 PNG 32 (RGBA), 1 PNG 24 (RGB on matte), 2 PNG 8 (indexed), 3 greyscale
    bool png16 = false;         // PNG 24 / 32 / greyscale: 16 bits per channel
    bool pngInterlace = false;  // PNG: Adam7 interlacing
    int pngCompression = 3;     // PNG: zlib level 0..9 (0.3.15: 3 was 6 — same pixels, faster)
    int pngTransparency = 2;    // PNG 8: 0 none (matte), 1 index transparency, 2 alpha transparency (tRNS)
    bool tinyPng = false;       // PNG / APNG: "Tiny compression" (libimagequant palette + tRNS, then zopfli)
    int tinyQualityMin = 65, tinyQualityMax = 80;   // libimagequant quality range 0..100
    int tinySpeed = 4;          // libimagequant speed 1 (best) .. 10 (fastest); >= 8 skips zopfli
    QVector<QRgb> customPalette; // palette "Custom" (GIF / PNG 8), opaque colours
    bool pngEditable = false;   // PNG: embed the editable document (Fireworks-style PNG, "lwDC" chunk)
    int bmpBits = 24;           // BMP: 1, 4, 8, 16 (RGB565), 24 or 32 (BGRA, BITMAPV5 with alpha)
    bool bmpRle = false;        // BMP: RLE4 / RLE8 compression for 4 / 8-bit
    int icoSizes = 0x4F;        // ICO / CUR: bit mask over ImageIO::iconSizes() (16, 24, 32, 48, 256)
    int icnsSizes = 0x1F5;      // ICNS: bit mask over ImageIO::iconSizes() (16, 32, 64, 128, 256, 512, 1024)
    int curHotX = 0, curHotY = 0; // CUR: hotspot, in document pixels
    int ddsFormat = 0;          // DDS / KTX2: 0 RGBA8 (uncompressed), 1 BC1 (DXT1), 2 BC3 (DXT5)
    bool mipmaps = false;       // DDS / KTX2: write a full mipmap chain
    bool ktxZstd = true;        // KTX2: Zstandard supercompression
    int ktxZstdLevel = 19;      // KTX2: zstd level 1..22
    int tinyDither = 100;       // PNG Tiny: libimagequant dither 0..100 %
    int exrCompression = 1;     // EXR: 0 none, 1 ZIP, 2 PIZ, 3 RLE, 4 ZIPS, 5 PXR24, 6 B44, 7 DWAA
    bool exrHalf = true;        // EXR: 16-bit half float (false = 32-bit float)
    bool pcx8 = false;          // PCX: 8-bit palette (false = 24-bit, or 32-bit with alpha)
    int jpegEncoder = 0;        // JPEG: 0 Standard (libjpeg-turbo), 1 MozJPEG (trellis), 2 jpegli
    bool jpegProgressive = false; // JPEG: progressive scans (SOF2)
    double dpi = 0;             // v0.3.1: resolution in pixels/inch; > 0 writes PNG pHYs / JPEG JFIF density
    bool jpegOptimize = false;  // JPEG: optimised Huffman tables
    int jpegSubsampling = 2;    // JPEG: 0 4:4:4, 1 4:2:2, 2 4:2:0
    int jpegSmoothing = 0;      // JPEG: Fireworks "Smoothing" 0..8 (input smoothing before DCT)
    bool jpegSharpenEdges = false; // JPEG: Fireworks "Sharpen color edges" (full-res chroma, edge-sharpened)
    bool jpegStripMeta = false; // JPEG: write no EXIF / XMP / comment markers
    bool jpegKeepIcc = true;    // JPEG: embed the ICC profile (when the image has one)
    // v0.2.6: C Header (.h)
    int chPixel = 0;            // cheader::Pixel (RGBA8888, ARGB8888, ..., RGB565 LE/BE, ..., 1-bit mono, indexed 8 / 4)
    int chElement = 0;          // 0 uint8_t byte array, 1 packed uint16_t / uint32_t words (16 / 32-bit formats)
    bool chHex = true;          // hex (true) or decimal values
    int chPerLine = 16;         // values per line
    bool chConst = true;        // const arrays
    bool chProgmem = false;     // AVR PROGMEM (defined empty elsewhere)
    bool chAligned = false;     // __attribute__((aligned(4)))
    bool chSection = false;     // __attribute__((section("chSectionName")))
    QString chSectionName = QStringLiteral(".rodata.images");
    bool chStdint = true;       // #include <stdint.h>
    bool chDefines = true;      // #define NAME_WIDTH ... (false: static const)
    int chFrameLayout = 0;      // multi-frame: 0 one array per frame, 1 one 2D array
    bool chDelays = true;       // multi-frame: a frame delays array (ms)
    bool chRle = false;         // PackBits RLE (byte arrays), with a decoder in the comment
    int chStyle = 0;            // 0 C arrays, 1 XBM-style (1-bit, LSB first, 1 = black), 2 GIMP C source (string), 3 GIMP header (HEADER_PIXEL)
    bool chRawBin = false;      // also write NAME.bin with the raw array bytes
    bool chLsbFirst = false;    // 1 / 4-bit formats: first pixel in the low bits
    bool chInvert = false;      // 1-bit: invert (C arrays: 1 = black; XBM: 1 = white)
    QString chName;             // C identifier; empty = from the file name (GIMP C source: the prefixed name)
    // v0.2.7: GIMP-style output (chStyle 2 / 3) and whitespace
    bool chGimpMacros = false;  // GIMP C source: NAME_WIDTH ... macros and NAME_pixel_data instead of a struct
    bool chGimpGlib = false;    // GIMP C source: guint8 / guint / gchar instead of unsigned char / unsigned int / char
    bool chGimpComment = false; // GIMP C source: save the comment
    QString chGimpCommentText = QStringLiteral("Created with ") + QStringLiteral(LW_PRODUCT_NAME);
    int chGimpOpacity = 100;    // GIMP C source / header: opacity 0..100 % (scales alpha, or blends onto the matte)
    bool chStrip = cheader_stripDefault();
    int chDither = 0;           // v0.3.0: fx2::DitherMethod applied before reducing to the pixel format (0 none) // "strip spaces/tabs/newlines from arrays": the initializer data on one line
    QString outputName;         // not saved: the output file name for GIMP's first comment line
    QString sourceName;         // not saved: the document name for the header comment
    QColor matte = Qt::white;   // used when the format has no alpha (or alpha is off)
    // v0.3.1 export scale (bitmap formats; after Export Area): percent of the canvas / area size,
    // resampled with a pixelscale Resample entry (-1 = Preferences > Interpolation, 4+ pixel-art scalers)
    int scalePct = 100;         // 1..1600
    int scaleResample = -1;
    // v0.3.1 Export Area: not saved (session only). When valid, every format writes only this
    // rectangle of the canvas (document px); empty = the whole canvas.
    QRect area;
    bool operator==(const ExportOptions &o) const;
    bool operator!=(const ExportOptions &o) const { return !(*this == o); }
};

struct AnimFrame {
    QImage image;               // ARGB32 / ARGB32_Premultiplied, full canvas size
    int delayMs = 100;
};

struct ImageSeq {
    QVector<AnimFrame> frames;
    int width = 0, height = 0;
    int loops = 0;              // 0 = forever
};

// HDR import (EXR / Radiance HDR / PFM / float TIFF-less sources): linear float RGBA pixels.
struct HdrImage {
    int width = 0, height = 0;
    QVector<float> rgba;        // linear, 4 floats per pixel, top-down
    bool isNull() const { return width <= 0 || height <= 0; }
};
// How HDR pixels become 8-bit sRGB on import.
struct ToneMap {
    enum Op { Clamp = 0, Reinhard = 1, ACES = 2, Exposure = 3 };   // Exposure: clamp after auto exposure
    int op = ACES;
    double exposure = 0;        // EV stops added before the operator
    bool operator==(const ToneMap &o) const { return op == o.op && exposure == o.exposure; }
};

namespace ImageIO {
QString formatName(Fmt f);
QString formatKey(Fmt f);
Fmt formatFromKey(const QString &k);
QString extension(Fmt f);
QList<Fmt> exportFormats();
QList<Fmt> importFormats();
bool canAnimate(Fmt f);
bool hasQuality(Fmt f);
bool hasLossless(Fmt f);
bool hasAlpha(const ExportOptions &o); // does this export keep transparency?
bool available(Fmt f);
Fmt detect(const QByteArray &head);
Fmt fromExtension(const QString &pathOrExt);

bool read(const QString &path, ImageSeq &out, QString *err = nullptr);
bool readData(const QByteArray &data, ImageSeq &out, QString *err = nullptr);
bool write(const QString &path, const ImageSeq &seq, const ExportOptions &opt, QString *err = nullptr);
// v0.3.1 export scale: every frame resized to pct % (at least 1 px) with a Resample entry (pixelscale)
ImageSeq scaleSeq(const ImageSeq &seq, int pct, int resample);
bool writeData(QByteArray &out, const ImageSeq &seq, const ExportOptions &opt, QString *err = nullptr);
// Export Area: the frames cut to `area` (intersected with the canvas). An empty `area` returns seq
// unchanged; ok = false when the area lies outside the canvas.
ImageSeq cropSeq(const ImageSeq &seq, const QRect &area, bool *ok = nullptr);

bool canRead(Fmt f);     // has a decoder in this build (PDF: no)
bool canWrite(Fmt f);    // has an encoder in this build (Camera RAW: no; HEIC: needs the x265 plugin)
bool isLayered(Fmt f);   // PSD / ORA / XCF / SVG / PDF / editable PNG: opened and exported as a document
bool isHdr(Fmt f);       // EXR / HDR / PFM: tone-mapped on import
const QVector<int> &iconSizes();   // 16, 24, 32, 48, 64, 128, 256, 512, 1024
QStringList extensions(Fmt f);     // every extension the format opens with
QStringList mimeTypes();           // MIME types of every importable format
QString mimeType(Fmt f);            // primary MIME type of a format (empty if none)

// Tone mapping used when HDR files are decoded to 8 bits.
ToneMap toneMap();
void setToneMap(const ToneMap &t);
QImage applyToneMap(const HdrImage &h, const ToneMap &t);
bool readHdr(const QByteArray &data, HdrImage &out, QString *err = nullptr); // EXR / HDR / PFM, linear

QString importFilter();
QString exportFilter(Fmt f);
QString codecSummary();
}
