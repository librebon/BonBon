#pragma once
#include "ImageIO.h"
#include <QPoint>
#include <QStringList>

// Internal per-format codecs. All return false and set *err on failure.
namespace codec {
// Layered raster image (PSD / ORA / XCF), bottom layer first.
struct RasterLayer {
    QString name;
    QImage image;            // ARGB32 (straight alpha), layer bounds
    QPoint offset;           // position of image(0,0) on the canvas
    int opacity = 255;       // 0..255
    int blend = 0;           // BlendMode value (Normal, Multiply, ...)
    bool visible = true;
    int group = -1;          // index into LayeredImage::groups of the innermost group, -1 = top level
};
// A layer group / folder (PSD section, ORA nested stack, XCF group layer), v0.3.2. Its members are
// the layers whose group chain reaches it; they are contiguous in the layer list.
struct LayerGroup {
    QString name;
    int opacity = 255;       // 0..255
    int blend = 0;           // BlendMode value (pass-through reads as Normal)
    bool visible = true;
    int parent = -1;         // enclosing group, -1 = top level
};
// the direct child of group `g` (-1 = top level) on the chain from group `grp` up; -1 when grp == g
int groupChildOf(const QVector<LayerGroup> &groups, int grp, int g);
// the layers as a tree, bottom first at every level: a node is a layer (layer >= 0) or a group
struct LayerNode { int layer = -1; int group = -1; QVector<LayerNode> children; };
struct LayeredImage;
QVector<LayerNode> layerTree(const LayeredImage &li);
struct LayeredImage {
    int width = 0, height = 0;
    QVector<RasterLayer> layers;
    QVector<LayerGroup> groups;
    QImage composite;        // the file's own flattened image (may be null)
    bool partial = false;    // something was skipped (adjustment layers, unsupported features)
    QStringList notes;
};
QImage compositeLayers(const LayeredImage &li);   // flatten with blend modes (transparent canvas)
// JPEG writer (libjpeg API; MozJPEG / jpegli are loaded at run time from the bundled private libraries).
bool writeJpeg(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
bool jpegEncoderAvailable(int encoder);   // 0 always; 1 MozJPEG; 2 jpegli
QString jpegEncoderVersion(int encoder);
bool readPsd(const QByteArray &d, LayeredImage &out, QString *err);
bool writePsd(QByteArray &out, const LayeredImage &li, QString *err);
bool readOra(const QByteArray &d, LayeredImage &out, QString *err);
bool writeOra(QByteArray &out, const LayeredImage &li, QString *err);
bool readXcf(const QByteArray &d, LayeredImage &out, QString *err);
bool writeXcf(QByteArray &out, const LayeredImage &li, QString *err);
bool readGif(const QByteArray &d, ImageSeq &out, QString *err);
bool writeGif(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool readWebp(const QByteArray &d, ImageSeq &out, QString *err);
bool writeWebp(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool isApng(const QByteArray &d);
bool readApng(const QByteArray &d, ImageSeq &out, QString *err);
bool writeApng(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool readAvif(const QByteArray &d, ImageSeq &out, QString *err);
bool writeAvif(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool readJxl(const QByteArray &d, ImageSeq &out, QString *err);
bool writeJxl(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool readNetpbm(const QByteArray &d, ImageSeq &out, QString *err);
bool writeNetpbm(QByteArray &out, const QImage &img, bool gray, bool ascii, QString *err);
bool readTiff(const QByteArray &d, ImageSeq &out, QString *err);
bool writeTiff(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool looksLikeTga(const QByteArray &d);
bool readTga(const QByteArray &d, ImageSeq &out, QString *err);
bool writeTga(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
// v0.2.5
bool readQoi(const QByteArray &d, ImageSeq &out, QString *err);
bool writeQoi(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
bool readBmp(const QByteArray &d, ImageSeq &out, QString *err);
bool writeBmp(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
// DIB (BITMAPINFOHEADER + pixels, no file header). icon = height is doubled and an AND mask follows.
bool readDib(const QByteArray &d, QImage &out, bool icon, QString *err);
QByteArray writeIconDib(const QImage &img);   // 32-bit BGRA DIB with AND mask (ICO entries <= 128 px)
bool readIco(const QByteArray &d, ImageSeq &out, QString *err);
bool writeIco(QByteArray &out, const QImage &img, const ExportOptions &o, bool cursor, QString *err);
bool readIcns(const QByteArray &d, ImageSeq &out, QString *err);
bool writeIcns(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
QImage iconCanvas(const QImage &img, int size);   // fit into a size x size transparent square
bool readPcx(const QByteArray &d, ImageSeq &out, QString *err);
bool writePcx(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
bool readPam(const QByteArray &d, ImageSeq &out, QString *err);
bool writePam(QByteArray &out, const QImage &img, bool alpha, QString *err);
bool writePbm(QByteArray &out, const QImage &img, bool ascii, bool dither, QString *err);
bool readPfm(const QByteArray &d, HdrImage &out, QString *err);
bool writePfm(QByteArray &out, const QImage &img, QString *err);
bool readRgbe(const QByteArray &d, HdrImage &out, QString *err);
bool writeRgbe(QByteArray &out, const QImage &img, QString *err);
bool readExr(const QByteArray &d, HdrImage &out, QString *err);
bool writeExr(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
bool readDds(const QByteArray &d, ImageSeq &out, QString *err);
bool writeDds(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
bool readKtx2(const QByteArray &d, ImageSeq &out, QString *err);
bool writeKtx2(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
bool readHeif(const QByteArray &d, ImageSeq &out, QString *err);
bool writeHeif(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err);
bool heifCanDecode();
bool heifCanEncode();
bool heifEncoderHasParameter(const QString &name);
QStringList heifEncoderParameterNames();
QString heifEncoderChromaParameterName();   // first parameter whose name contains "chroma", or empty
void heifInitPlugins();   // load the bundled libheif codec plugins (AppImage / tarball)
bool readRaw(const QByteArray &d, ImageSeq &out, QString *err);
QString rawLibraryVersion();
bool writePng(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err);
// The colour table an indexed export (GIF / PNG 8) would use; transparent entries have alpha < 255.
QVector<QRgb> indexedPalette(const QImage &img, const ExportOptions &o);
// Tiny compression: lossy palette quantisation (libimagequant, full alpha) + lossless recompression.
// Falls back to a lossless PNG 32 when the minimum quality cannot be met. *psnr receives the PSNR (dB)
// of the quantised image against the source (RGBA), when non-null.
bool writeTinyPng(QByteArray &out, const QImage &img, const ExportOptions &o, QString *err, double *psnr = nullptr);
// Quantise frames to one shared palette (APNG tiny compression); returns false without libimagequant.
bool tinyQuantizeFrames(QVector<QImage> &frames, const ExportOptions &o, QString *err);
QByteArray optimizePngLossless(const QByteArray &png, int speed);   // zopflipng; returns the input when not smaller
bool tinyAvailable();
bool isFireworksPng(const QByteArray &d);   // has Macromedia Fireworks private chunks (mkBF / mkBT ...)
QByteArray pngChunk(const QByteArray &png, const char type[4]);            // first chunk payload, or empty
QByteArray pngWithChunk(const QByteArray &png, const char type[4], const QByteArray &payload); // insert before IEND
bool rasterizeSvg(const QByteArray &d, ImageSeq &out, QString *err);
// sRGB <-> linear helpers (HDR)
float srgbToLinear(float v);
float linearToSrgb(float v);
QImage flatten(const QImage &img, const QColor &matte);
inline void setErr(QString *e, const QString &m) { if (e) *e = m; }
// v0.2.6: C Header (.h) export
struct CHeaderData {
    int pixel = 0, width = 0, height = 0, stride = 0;
    QVector<QByteArray> raw;      // unpacked pixel bytes per frame (byte layout)
    QVector<QByteArray> stored;   // what the arrays hold (RLE-packed when RLE is on)
    QVector<QImage> decoded;      // what the target will show (quantised), per frame
    QVector<QRgb> palette;        // indexed formats
    int transparentIndex = -1;
    QVector<int> delays;
    qint64 dataBytes = 0;         // pixel array bytes (all frames)
    QByteArray bin;               // the raw .bin written alongside
};
bool encodeCHeader(const ImageSeq &seq, const ExportOptions &o, CHeaderData &d, QString *err);
bool writeCHeader(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err, CHeaderData *info = nullptr);
}

namespace cheader {
enum Pixel { RGBA8888 = 0, ARGB8888, BGRA8888, RGB888, RGB565LE, RGB565BE, RGB555, ARGB1555, RGBA4444, Grey8, Grey4, Mono1, Indexed8, Indexed4, PixelCount };
QStringList pixelNames();
int bitsPerPixel(int pixel);
bool hasAlpha(int pixel);
bool isIndexed(int pixel);
int wordBits(int pixel);              // 16 / 32 when packed words are possible, else 0
int stride(int pixel, int width);     // bytes per row (sub-byte rows padded)
QString identifier(const QString &name);   // a valid C identifier from a file name
QByteArray unpackBits(const QByteArray &in, int expected = -1);
// v0.2.7 GIMP-style output
int gimpBytesPerPixel(int pixel);     // GIMP C source: 2 (RGB565 LE / BE), 4 (formats with alpha), else 3
int gimpPixel(int style, int pixel);  // the Pixel written by chStyle 2 / 3 (RGB565LE, RGBA8888 or RGB888)
QByteArray gimpRle(const QByteArray &in, int bpp);   // GIMP's 1-byte run-length encoding (rl_encode_rgbx)
QByteArray gimpRleDecode(const QByteArray &in, int bpp, int expected = -1);
}
