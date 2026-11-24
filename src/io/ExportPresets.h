#pragma once
// Export presets (Optimize panel "Settings", the export dialog and the batch processor).
#include "ImageIO.h"
#include "Codecs.h"
#include <QStringList>
#include <iterator>

namespace exportpresets {
struct Preset {
    const char *name; Fmt f; int q; bool lossless; int colors; int palette; bool dither; bool anim; bool ascii;
    int tiffComp = 1; bool b16 = false; bool alpha = true; bool rle = true;   // TIFF / TGA
    int pngType = 0; int jpegEnc = 0; bool jpegProg = false; bool jpegOpt = false;   // v0.2.5
    int chPixel = 0; int chElement = 0;   // v0.2.6: C Header
    int chStyle = 0;                      // v0.2.7: 2 = GIMP C source (string)
};
inline const Preset kPresets[] = {
    {"GIF Web 216", Fmt::GIF, 80, false, 216, 1, false, false, false},
    {"GIF Adaptive 256", Fmt::GIF, 80, false, 256, 0, true, false, false},
    {"GIF Adaptive 64 (smaller)", Fmt::GIF, 80, false, 64, 0, true, false, false},
    {"Animated GIF Adaptive 256", Fmt::GIF, 80, false, 256, 0, true, true, false},
    {"JPEG - Better Quality", Fmt::JPEG, 80, false, 256, 0, false, false, false},
    {"JPEG - Smaller File", Fmt::JPEG, 60, false, 256, 0, false, false, false},
    {"JPEG - Progressive Optimised", Fmt::JPEG, 80, false, 256, 0, false, false, false, 1, false, true, true, 0, 0, true, true},
    {"JPEG - jpegli 85", Fmt::JPEG, 85, false, 256, 0, false, false, false, 1, false, true, true, 0, 2, true, true},
    {"PNG 8 Adaptive 256", Fmt::PNG, 80, false, 256, 0, true, false, false, 1, false, true, true, 2},
    {"PNG 8 Web 216", Fmt::PNG, 80, false, 216, 1, false, false, false, 1, false, true, true, 2},
    {"PNG 24", Fmt::PNG, 80, false, 256, 0, false, false, false, 1, false, true, true, 1},
    {"PNG 32", Fmt::PNG, 80, false, 256, 0, false, false, false},
    {"Animated PNG (APNG)", Fmt::APNG, 80, false, 256, 0, false, true, false},
    {"WebP Lossy 80", Fmt::WEBP, 80, false, 256, 0, false, false, false},
    {"WebP Lossless", Fmt::WEBP, 100, true, 256, 0, false, false, false},
    {"Animated WebP", Fmt::WEBP, 80, false, 256, 0, false, true, false},
    {"AVIF Quality 60", Fmt::AVIF, 60, false, 256, 0, false, false, false},
    {"AVIF Lossless", Fmt::AVIF, 100, true, 256, 0, false, false, false},
    {"JPEG XL Quality 85", Fmt::JXL, 85, false, 256, 0, false, false, false},
    {"JPEG XL Lossless", Fmt::JXL, 100, true, 256, 0, false, false, false},
    {"PPM (binary P6)", Fmt::PPM, 80, false, 256, 0, false, false, false},
    {"PGM (binary P5)", Fmt::PGM, 80, false, 256, 0, false, false, false},
    {"PPM (ASCII P3)", Fmt::PPM, 80, false, 256, 0, false, false, true},
    {"PGM (ASCII P2)", Fmt::PGM, 80, false, 256, 0, false, false, true},
    {"TIFF LZW", Fmt::TIFF, 80, false, 256, 0, false, false, false, 1, false, true, true},
    {"TIFF Deflate (ZIP)", Fmt::TIFF, 80, false, 256, 0, false, false, false, 2, false, true, true},
    {"TIFF 16-bit Deflate", Fmt::TIFF, 80, false, 256, 0, false, false, false, 2, true, true, true},
    {"TIFF Uncompressed", Fmt::TIFF, 80, false, 256, 0, false, false, false, 0, false, true, true},
    {"Multi-page TIFF LZW", Fmt::TIFF, 80, false, 256, 0, false, true, false, 1, false, true, true},
    {"TGA 32-bit RLE", Fmt::TGA, 80, false, 256, 0, false, false, false, 1, false, true, true},
    {"TGA 24-bit RLE", Fmt::TGA, 80, false, 256, 0, false, false, false, 1, false, false, true},
    {"TGA 32-bit Uncompressed", Fmt::TGA, 80, false, 256, 0, false, false, false, 1, false, true, false},
    {"C Header RGBA8888", Fmt::CHeader, 80, false, 256, 0, false, false, false, 1, false, true, true, 0, 0, false, false, cheader::RGBA8888, 0},
    {"C Header RGB565 (embedded)", Fmt::CHeader, 80, false, 256, 0, false, false, false, 1, false, true, true, 0, 0, false, false, cheader::RGB565LE, 1},
    {"C Header 1-bit mono (OLED)", Fmt::CHeader, 80, false, 256, 0, false, false, false, 1, false, true, true, 0, 0, false, false, cheader::Mono1, 0},
    {"C Header Indexed 8-bit", Fmt::CHeader, 80, false, 256, 0, true, false, false, 1, false, true, true, 0, 0, false, false, cheader::Indexed8, 0},
    {"C Header GIMP-style RGBA (short string)", Fmt::CHeader, 80, false, 256, 0, false, false, false, 1, false, true, true, 0, 0, false, false, cheader::RGBA8888, 0, 2},
    {"C Header GIMP-style RGB565 RLE (short string)", Fmt::CHeader, 80, false, 256, 0, false, false, false, 1, false, true, true, 0, 0, false, false, cheader::RGB565LE, 0, 2},
};

constexpr int kCount = int(std::size(kPresets));
QStringList names();
int indexOf(const QString &name);   // case-insensitive; -1 = none
// Applies preset `i` (0-based) over `o` (keeps everything the preset does not set).
ExportOptions apply(int i, ExportOptions o);
}
