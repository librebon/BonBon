#include "Brand.h"
// C Header (.h) export: the image as C arrays for embedded / firmware use.
#include "Codecs.h"
#include "Quantize.h"
#include "core/FxExtra.h"
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <cstring>

namespace {
using CP = cheader::Pixel;

struct PixInfo { const char *name; int bpp; int wordBits; bool alpha; const char *layout; };
const PixInfo kPix[] = {
    {"RGBA8888", 32, 32, true, "bytes R,G,B,A; words 0xRRGGBBAA"},
    {"ARGB8888", 32, 32, true, "bytes A,R,G,B; words 0xAARRGGBB"},
    {"BGRA8888", 32, 32, true, "bytes B,G,R,A; words 0xBBGGRRAA"},
    {"RGB888", 24, 0, false, "bytes R,G,B"},
    {"RGB565 (little-endian)", 16, 16, false, "RRRRRGGG GGGBBBBB; bytes low, high; words = the 565 value"},
    {"RGB565 (big-endian)", 16, 16, false, "RRRRRGGG GGGBBBBB; bytes high, low; words byte-swapped (big-endian in memory on little-endian CPUs)"},
    {"RGB555", 16, 16, false, "0RRRRRGG GGGBBBBB; bytes low, high"},
    {"ARGB1555", 16, 16, true, "ARRRRRGG GGGBBBBB (A: alpha >= 128); bytes low, high"},
    {"RGBA4444", 16, 16, true, "RRRRGGGG BBBBAAAA; bytes low, high"},
    {"Grey8", 8, 0, false, "one byte per pixel, 0 black .. 255 white"},
    {"Grey4", 4, 0, false, "two pixels per byte (0 black .. 15 white), rows padded to a byte"},
    {"1-bit mono", 1, 0, false, "eight pixels per byte, rows padded to a byte"},
    {"Indexed 8-bit", 8, 0, true, "one palette index per pixel"},
    {"Indexed 4-bit", 4, 0, true, "two palette indices per byte, rows padded to a byte"},
};

const char *kKeywords[] = {"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
                           "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
                           "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
                           "bool", "true", "false", "class", "new", "delete", "this", "template", "namespace", "private", "public"};

inline int luma(QRgb c) { return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114 + 500) / 1000; }

// PackBits: control n < 128: n + 1 literal bytes follow; n >= 128: the next byte repeats n - 125 times (3..130).
QByteArray packBits(const QByteArray &in) {
    QByteArray out;
    const int n = int(in.size());
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 130 && in[i + run] == in[i]) ++run;
        if (run >= 3) { out.append(char(run + 125)); out.append(in[i]); i += run; continue; }
        int start = i, len = 0;
        while (i < n && len < 128) {
            int r = 1;
            while (i + r < n && r < 3 && in[i + r] == in[i]) ++r;
            if (r >= 3) break;
            ++i; ++len;
        }
        out.append(char(len - 1));
        out.append(in.mid(start, len));
    }
    return out;
}
}

namespace cheader {
QStringList pixelNames() { QStringList l; for (const auto &p : kPix) l << QString::fromLatin1(p.name); return l; }
int bitsPerPixel(int pixel) { return kPix[qBound(0, pixel, int(std::size(kPix)) - 1)].bpp; }
bool hasAlpha(int pixel) { return kPix[qBound(0, pixel, int(std::size(kPix)) - 1)].alpha; }
bool isIndexed(int pixel) { return pixel == Indexed8 || pixel == Indexed4; }
int wordBits(int pixel) { return kPix[qBound(0, pixel, int(std::size(kPix)) - 1)].wordBits; }
int stride(int pixel, int width) { return (width * bitsPerPixel(pixel) + 7) / 8; }

QString identifier(const QString &name) {
    QString s;
    for (QChar c : name) s += (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('_') ? c : QLatin1Char('_');
    s.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    while (s.size() > 1 && s.endsWith(QLatin1Char('_'))) s.chop(1);
    if (s.isEmpty() || s == QLatin1String("_")) s = QStringLiteral("image");
    if (s[0].isDigit()) s.prepend(QStringLiteral("img_"));
    for (const char *k : kKeywords) if (s == QLatin1String(k)) { s += QLatin1Char('_'); break; }
    return s;
}

QByteArray unpackBits(const QByteArray &in, int expected) {
    QByteArray out;
    int i = 0;
    while (i < in.size() && (expected < 0 || out.size() < expected)) {
        const int c = uchar(in[i++]);
        if (c < 128) { out.append(in.mid(i, c + 1)); i += c + 1; }
        else if (i < in.size()) { out.append(QByteArray(c - 125, in[i++])); }
    }
    return out;
}

int gimpBytesPerPixel(int pixel) {
    if (pixel == RGB565LE || pixel == RGB565BE) return 2;
    return hasAlpha(pixel) ? 4 : 3;
}
int gimpPixel(int style, int pixel) {
    if (style == 3) return RGB888;
    const int b = gimpBytesPerPixel(pixel);
    return b == 2 ? RGB565LE : b == 4 ? RGBA8888 : RGB888;
}

// GIMP file-csource rl_encode_rgbx: a count byte l < 128 is followed by l literal pixels, l | 128 (l = 2..127) by one
// pixel repeated l times. Same output as GIMP, except that a single-pixel image (where GIMP asserts) gives 1, pixel.
QByteArray gimpRle(const QByteArray &in, int bpp) {
    QByteArray out;
    const uchar *ip = reinterpret_cast<const uchar *>(in.constData());
    const uchar *limit = ip + (in.size() / bpp) * bpp, *ilimit = limit - bpp;
    auto diff2 = [bpp](const uchar *p) { return std::memcmp(p, p + bpp, size_t(bpp)) != 0; };
    auto put = [&](const uchar *p, int n) { out.append(reinterpret_cast<const char *>(p), n); };
    while (ip < limit) {
        if (ip == ilimit) { out.append(char(1)); put(ip, bpp); break; }
        if (diff2(ip)) {
            const uchar *sip = ip;
            int l = 1;
            ip += bpp;
            while (l < 127 && ip < ilimit && diff2(ip)) { ip += bpp; ++l; }
            if (ip == ilimit && l < 127) { ip += bpp; ++l; }
            out.append(char(l));
            put(sip, l * bpp);
        } else {
            int l = 2;
            ip += bpp;
            while (l < 127 && ip < ilimit && !diff2(ip)) { ip += bpp; ++l; }
            out.append(char(l | 128));
            put(ip, bpp);
            ip += bpp;
        }
        if (ip == ilimit) { out.append(char(1)); put(ip, bpp); ip += bpp; }
    }
    return out;
}

QByteArray gimpRleDecode(const QByteArray &in, int bpp, int expected) {
    QByteArray out;
    int i = 0;
    while (i < in.size() && (expected < 0 || out.size() < expected)) {
        const int c = uchar(in[i++]);
        if (c & 128) { const QByteArray px = in.mid(i, bpp); i += bpp; for (int k = 0; k < (c & 127); ++k) out += px; }
        else { out += in.mid(i, c * bpp); i += c * bpp; }
    }
    return out;
}
}

namespace {
// GIMP C source pixel bytes as C string literal text. Shorter than GIMP's own escaping: octal escapes use the fewest
// digits (3 only when an octal digit follows), digits and other printable ASCII stay as they are, and a '?' that
// follows a '?' becomes \? so no trigraph (??/ ??= ...) can form. Not stripping: GIMP's line wrap after column 74.
struct CStringWriter {
    QByteArray s;
    bool strip = true;
    QByteArray brk;   // the line break text ("\"\n  \"")
    int startCol = 3, col = 3;
    char last = 0;    // the last source character written inside the literal
    void wrap() { if (!strip && col > 74) { s += brk; col = startCol; last = 0; } }
    void putText(const QByteArray &t) { s += t; col += int(t.size()); last = t.isEmpty() ? last : t.back(); }
    void bytes(const QByteArray &data) {
        const int n = int(data.size());
        for (int i = 0; i < n; ++i) {
            wrap();
            const int d = uchar(data[i]);
            const int next = i + 1 < n ? uchar(data[i + 1]) : -1;
            if (d < 33 || d > 126) {
                QByteArray e = QByteArray::number(d, 8);
                if (next >= '0' && next <= '7') e = e.rightJustified(3, '0');
                putText("\\" + e);
            } else if (d == '\\') putText("\\\\");
            else if (d == '"') putText("\\\"");
            else if (d == '?' && last == '?') putText("\\?");
            else putText(QByteArray(1, char(d)));
        }
    }
};

// GIMP's comment escaping (\\ \" \n \r \b \f, other non-printables as 3-digit octal of the unsigned byte), with
// GIMP's line split at each newline when not stripping, and the same trigraph guard.
QByteArray cComment(const QByteArray &text, bool strip, bool macros) {
    QByteArray s;
    char last = 0;
    for (int i = 0; i < text.size(); ++i) {
        const uchar c = uchar(text[i]);
        QByteArray e;
        if (c == '\\') e = "\\\\";
        else if (c == '"') e = "\\\"";
        else if (c == '\n' && i + 1 < text.size() && !strip) { s += QByteArray("\\n\"") + (macros ? " \\" : "") + "\n  \""; last = 0; continue; }
        else if (c == '\n') e = "\\n";
        else if (c == '\r') e = "\\r";
        else if (c == '\b') e = "\\b";
        else if (c == '\f') e = "\\f";
        else if (c == '?' && last == '?') e = "\\?";
        else if ((c >= 32 && c <= 47) || (c >= 58 && c <= 126)) e = QByteArray(1, char(c));
        else e = "\\" + QByteArray::number(c, 8).rightJustified(3, '0');
        s += e;
        last = e.back();
    }
    return s;
}

QByteArray gimpRleDecoder(const QByteArray &M, const QByteArray &sUint, const QByteArray &sUint8) {
    QByteArray r;
    r += "#define " + M + "_RUN_LENGTH_DECODE(image_buf, rle_data, size, bpp) do \\\n";
    r += "{ " + sUint + " __bpp; " + sUint8 + " *__ip; const " + sUint8 + " *__il, *__rd; \\\n";
    r += "  __bpp = (bpp); __ip = (image_buf); __il = __ip + (size) * __bpp; \\\n"
         "  __rd = (rle_data); if (__bpp > 3) { /* RGBA */ \\\n"
         "    while (__ip < __il) { " + sUint + " __l = *(__rd++); \\\n";
    r += "      if (__l & 128) { __l = __l - 128; \\\n"
         "        do { memcpy (__ip, __rd, 4); __ip += 4; } while (--__l); __rd += 4; \\\n"
         "      } else { __l *= 4; memcpy (__ip, __rd, __l); \\\n"
         "               __ip += __l; __rd += __l; } } \\\n"
         "  } else if (__bpp == 3) { /* RGB */ \\\n"
         "    while (__ip < __il) { " + sUint + " __l = *(__rd++); \\\n";
    r += "      if (__l & 128) { __l = __l - 128; \\\n"
         "        do { memcpy (__ip, __rd, 3); __ip += 3; } while (--__l); __rd += 3; \\\n"
         "      } else { __l *= 3; memcpy (__ip, __rd, __l); \\\n"
         "               __ip += __l; __rd += __l; } } \\\n"
         "  } else { /* RGB16 */ \\\n"
         "    while (__ip < __il) { " + sUint + " __l = *(__rd++); \\\n";
    r += "      if (__l & 128) { __l = __l - 128; \\\n"
         "        do { memcpy (__ip, __rd, 2); __ip += 2; } while (--__l); __rd += 2; \\\n"
         "      } else { __l *= 2; memcpy (__ip, __rd, __l); \\\n"
         "               __ip += __l; __rd += __l; } } \\\n"
         "  } } while (0)\n";
    return r;
}

// GIMP header (file-header.c): each RGB pixel as four characters 33..96
QByteArray headerChars(const QByteArray &rgb) {
    QByteArray out;
    const uchar *p = reinterpret_cast<const uchar *>(rgb.constData());
    for (int i = 0; i + 2 < rgb.size(); i += 3) {
        const int d0 = p[i], d1 = p[i + 1], d2 = p[i + 2];
        out.append(char(((d0 >> 2) & 0x3F) + 33));
        out.append(char((((d0 & 0x3) << 4) | (d1 >> 4)) + 33));
        out.append(char((((d1 & 0xF) << 2) | (d2 >> 6)) + 33));
        out.append(char((d2 & 0x3F) + 33));
    }
    return out;
}

bool writeGimp(QByteArray &out, const codec::CHeaderData &d, const ExportOptions &o) {
    const bool strip = o.chStrip;
    const QByteArray base = (o.outputName.isEmpty() ? cheader::identifier(o.chName.isEmpty() ? QStringLiteral("image") : o.chName) + QStringLiteral(".h") : o.outputName).toUtf8();
    const QByteArray &st = d.stored.value(0);
    QByteArray h;
    if (o.chStyle == 3) {
        h += "/*  GIMP header image file format (RGB): " + QByteArray(base).replace("*/", "* /") + "  */\n\n";
        h += "static unsigned int width = " + QByteArray::number(d.width) + ";\n";
        h += "static unsigned int height = " + QByteArray::number(d.height) + ";\n\n";
        h += "/*  Call this macro repeatedly.  After each use, the pixel data can be extracted  */\n\n"
             "#define HEADER_PIXEL(data,pixel) {\\\n"
             "pixel[0] = (((data[0] - 33) << 2) | ((data[1] - 33) >> 4)); \\\n"
             "pixel[1] = ((((data[1] - 33) & 0xF) << 4) | ((data[2] - 33) >> 2)); \\\n"
             "pixel[2] = ((((data[2] - 33) & 0x3) << 6) | ((data[3] - 33))); \\\n"
             "data += 4; \\\n"
             "}\n";
        h += strip ? "static char *header_data =\n\"" : "static char *header_data =\n\t\"";
        char last = 0;
        for (int i = 0; i < st.size(); ++i) {
            if (!strip && i > 0 && i % 64 == 0) { h += "\"\n\t\""; last = 0; }   // 16 pixels per line, as GIMP
            const char c = st[i];
            if (c == '"' || c == '\\') { h += '\\'; h += c; }
            else if (c == '?' && last == '?') h += "\\?";
            else h += c;
            last = c;
        }
        h += "\";\n";
        out = h;
        return true;
    }
    const bool macros = o.chGimpMacros, glib = o.chGimpGlib, rle = o.chRle;
    const int bpp = cheader::gimpBytesPerPixel(d.pixel);
    const QByteArray name = cheader::identifier(o.chName.isEmpty() ? QStringLiteral("gimp_image") : o.chName).toLatin1();
    const QByteArray M = name.toUpper();
    QByteArray sUint8, sUint, sChar, sNull;
    if (!macros) {
        sUint8 = glib ? "guint8 " : "unsigned char"; sUint = glib ? "guint  " : "unsigned int ";
        sChar = glib ? "gchar  " : "char         "; sNull = glib ? "NULL" : "(char*) 0";
    } else {
        sUint8 = glib ? "guint8" : "unsigned char"; sUint = glib ? "guint" : "unsigned int";
        sChar = glib ? "gchar" : "char"; sNull = glib ? "NULL" : "(char*) 0";
    }
    const QByteArray W = QByteArray::number(d.width), H = QByteArray::number(d.height), B = QByteArray::number(bpp);
    const QByteArray size = rle ? QByteArray::number(st.size()) + " + 1" : W + " * " + H + " * " + B + " + 1";
    const QByteArray comment = o.chGimpCommentText.toUtf8();
    h += QByteArray("/* GIMP ") + (bpp == 4 ? "RGBA" : "RGB") + " C-Source image dump " + (rle ? "1-byte-run-length-encoded " : "") + "(" + QByteArray(base).replace("*/", "* /") + ") */\n\n";
    const QByteArray macrosUint = glib ? "guint" : "unsigned int", macrosUint8 = glib ? "guint8" : "unsigned char";
    if (rle && !macros) h += gimpRleDecoder(M, macrosUint, macrosUint8);
    if (!macros) {
        h += "static const struct {\n  " + sUint + "\t width;\n  " + sUint + "\t height;\n  " + sUint + "\t bytes_per_pixel; /* 2:RGB16, 3:RGB, 4:RGBA */ \n";
        if (o.chGimpComment) h += "  " + sChar + "\t*comment;\n";
        h += "  " + sUint8 + "\t " + (rle ? "rle_" : "") + "pixel_data[" + size + "];\n";
        h += "} " + name + " = {\n  " + W + ", " + H + ", " + B + ",\n";
    } else {
        h += "#define " + M + "_WIDTH (" + W + ")\n#define " + M + "_HEIGHT (" + H + ")\n#define " + M + "_BYTES_PER_PIXEL (" + B + ") /* 2:RGB16, 3:RGB, 4:RGBA */\n";
    }
    if (o.chGimpComment && comment.isEmpty()) h += macros ? "#define " + M + "_COMMENT (" + sNull + ")\n" : "  " + sNull + ",\n";
    else if (o.chGimpComment) {
        if (macros) h += "#define " + M + "_COMMENT \\\n";
        h += "  \"" + cComment(comment, strip, macros) + (macros ? "\"\n" : "\",\n");
    }
    CStringWriter w;
    w.strip = strip;
    if (macros) {
        h += "#define " + M + "_" + (rle ? "RLE_" : "") + "PIXEL_DATA ((" + sUint8 + "*) " + M + "_" + (rle ? "rle_" : "") + "pixel_data)\n";
        if (rle) h += gimpRleDecoder(M, sUint, sUint8);
        h += "static const " + sUint8 + " " + M + "_" + (rle ? "rle_" : "") + "pixel_data[" + size + "] =\n(\"";
        w.brk = "\"\n \""; w.startCol = w.col = 2;
    } else {
        h += "  \"";
        w.brk = "\"\n  \""; w.startCol = w.col = 3;
    }
    w.bytes(st);
    h += w.s;
    h += macros ? "\");\n\n" : "\",\n};\n\n";
    out = h;
    return true;
}
}

namespace codec {

// GIMP C source / GIMP header: one frame; RGB565 LE, RGBA or RGB with GIMP's opacity rules. RGB / RGB565 are blended
// onto the matte (black = exactly GIMP's output); RGBA keeps the colour and scales alpha.
static bool encodeGimp(const ImageSeq &seqIn, const ExportOptions &o, CHeaderData &d) {
    const int pixel = cheader::gimpPixel(o.chStyle, o.chPixel);
    const int bpp = o.chStyle == 3 ? 3 : cheader::gimpBytesPerPixel(pixel);
    const QImage im = seqIn.frames[0].image.convertToFormat(QImage::Format_ARGB32);
    d.pixel = pixel;
    d.width = im.width();
    d.height = im.height();
    d.stride = d.width * bpp;
    d.delays << seqIn.frames[0].delayMs;
    const double op = qBound(0, o.chGimpOpacity, 100);
    const int mr = qRed(o.matte.rgb()), mg = qGreen(o.matte.rgb()), mb = qBlue(o.matte.rgb());
    QByteArray raw(d.stride * d.height, 0);
    QImage dec(d.width, d.height, QImage::Format_ARGB32);
    uchar *p = reinterpret_cast<uchar *>(raw.data());
    for (int y = 0; y < d.height; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
        QRgb *dl = reinterpret_cast<QRgb *>(dec.scanLine(y));
        for (int x = 0; x < d.width; ++x) {
            const QRgb c = s[x];
            if (bpp == 4) {
                const int a = int(std::floor(qAlpha(c) * op / 100.0 + 0.5));
                *p++ = uchar(qRed(c)); *p++ = uchar(qGreen(c)); *p++ = uchar(qBlue(c)); *p++ = uchar(a);
                dl[x] = qRgba(qRed(c), qGreen(c), qBlue(c), a);
                continue;
            }
            const double f = qAlpha(c) * op / 25500.0;
            auto blend = [f](int v, int m) { return int(std::floor(0.5 + f * v + (1.0 - f) * m)); };
            const int r = blend(qRed(c), mr), g = blend(qGreen(c), mg), b = blend(qBlue(c), mb);
            if (bpp == 3) { *p++ = uchar(r); *p++ = uchar(g); *p++ = uchar(b); dl[x] = qRgb(r, g, b); continue; }
            const int R = r >> 3, G = g >> 2, B = b >> 3;
            const quint16 v = quint16((R << 11) + (G << 5) + B);
            *p++ = uchar(v & 0xFF); *p++ = uchar(v >> 8);
            dl[x] = qRgb((R << 3) | (R >> 2), (G << 2) | (G >> 4), (B << 3) | (B >> 2));
        }
    }
    d.raw << raw;
    d.decoded << dec;
    return true;
}

// v0.3.0: dither the frames to the pixel format's channel depths before they are truncated
// (indexed formats dither to their palette in encodeCHeader instead).
static ImageSeq ditherForPixel(const ImageSeq &in, const ExportOptions &o, int pixel) {
    int r = 8, g = 8, b = 8, a = 8;
    bool grey = false;
    switch (pixel) {
    case int(CP::RGB565LE): case int(CP::RGB565BE): r = 5; g = 6; b = 5; break;
    case int(CP::RGB555): r = g = b = 5; break;
    case int(CP::ARGB1555): r = g = b = 5; a = 1; break;
    case int(CP::RGBA4444): r = g = b = a = 4; break;
    case int(CP::Grey4): r = g = b = 4; grey = true; break;
    case int(CP::Mono1): r = g = b = 1; grey = true; break;
    default: return in;
    }
    ImageSeq out = in;
    for (AnimFrame &f : out.frames) {
        QImage im = f.image.convertToFormat(QImage::Format_ARGB32);
        if (!cheader::hasAlpha(pixel)) im = flatten(im, o.matte);
        f.image = fx2::ditherToBits(im, o.chDither, r, g, b, grey, a).convertToFormat(QImage::Format_ARGB32);
    }
    return out;
}

bool encodeCHeader(const ImageSeq &seqOrig, const ExportOptions &o, CHeaderData &d, QString *err) {
    d = CHeaderData();
    if (seqOrig.frames.isEmpty()) { setErr(err, QStringLiteral("Nothing to export")); return false; }
    const bool ditherOn = o.chDither > 0 && o.chDither < fx2::kDitherMethods;
    const int ditherPixel = o.chStyle >= 2 ? cheader::gimpPixel(o.chStyle, o.chPixel) : o.chStyle == 1 ? int(CP::Mono1) : o.chPixel;
    const ImageSeq seqIn = ditherOn && !cheader::isIndexed(ditherPixel) ? ditherForPixel(seqOrig, o, ditherPixel) : seqOrig;
    if (o.chStyle >= 2) return encodeGimp(seqIn, o, d);

    const bool xbm = o.chStyle == 1;
    const int pixel = xbm ? int(CP::Mono1) : qBound(0, o.chPixel, int(std::size(kPix)) - 1);
    const bool lsb = xbm ? true : o.chLsbFirst;
    const bool invert = xbm ? !o.chInvert : o.chInvert;   // XBM: 1 = black (foreground)
    d.pixel = pixel;
    d.width = seqIn.frames[0].image.width();
    d.height = seqIn.frames[0].image.height();
    const int W = d.width, H = d.height, st = cheader::stride(pixel, W);
    d.stride = st;
    QVector<QImage> imgs;
    for (const auto &f : seqIn.frames) {
        QImage im = f.image.convertToFormat(QImage::Format_ARGB32);
        if (!cheader::hasAlpha(pixel)) im = flatten(im, o.matte);
        imgs << im;
        d.delays << f.delayMs;
    }
    // indexed: one palette for all frames (GIF / PNG 8 quantiser), slot 0 transparent if needed
    QVector<QVector<uchar>> idx;
    if (cheader::isIndexed(pixel)) {
        const int maxC = pixel == int(CP::Indexed4) ? qMin(16, o.colors) : qBound(2, o.colors, 256);
        bool transparent = false;
        for (const QImage &im : imgs) {
            for (int y = 0; y < H && !transparent; ++y) {
                const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
                for (int x = 0; x < W; ++x) if (qAlpha(s[x]) < 128) { transparent = true; break; }
            }
        }
        QVector<QImage> opaque;
        for (const QImage &im : imgs) { QImage c = im; for (int y = 0; y < H; ++y) { QRgb *s = reinterpret_cast<QRgb *>(c.scanLine(y)); for (int x = 0; x < W; ++x) s[x] |= 0xFF000000u; } opaque << c; }
        QVector<QRgb> pal = quant::buildPalette(opaque, qMax(2, maxC - (transparent ? 1 : 0)), o.palette, o.customPalette);
        if (pal.isEmpty()) pal << qRgb(0, 0, 0);
        for (const QImage &im : imgs) {
            if (ditherOn) {   // v0.3.0: the chosen dither method onto the palette, then an exact mapping
                const QImage dm = fx2::dither(im, o.chDither, fx2::PalCustom, int(pal.size()), pal, 100, true).convertToFormat(QImage::Format_ARGB32);
                idx << quant::mapImage(dm, pal, 0, 0, transparent ? 0 : -1, transparent ? 1 : 0);
            } else {
                idx << quant::mapImage(im, pal, o.dither ? o.ditherAmount : 0, o.loss, transparent ? 0 : -1, transparent ? 1 : 0);
            }
        }
        if (transparent) { pal.prepend(qRgba(0, 0, 0, 0)); d.transparentIndex = 0; }
        d.palette = pal;
    }
    for (int fi = 0; fi < imgs.size(); ++fi) {
        const QImage &im = imgs[fi];
        QByteArray raw(st * H, 0);
        QImage dec(W, H, QImage::Format_ARGB32);
        QVector<uchar> mono;
        if (pixel == int(CP::Mono1)) {
            mono.resize(W * H);
            if (o.dither && o.ditherAmount > 0) {
                const QVector<uchar> m = quant::mapImage(im, {qRgb(0, 0, 0), qRgb(255, 255, 255)}, o.ditherAmount, 0, -1, 0);
                for (int i = 0; i < W * H; ++i) mono[i] = m[i];
            } else
                for (int y = 0; y < H; ++y) {
                    const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
                    for (int x = 0; x < W; ++x) mono[y * W + x] = luma(s[x]) >= 128 ? 1 : 0;
                }
        }
        for (int y = 0; y < H; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
            QRgb *dl = reinterpret_cast<QRgb *>(dec.scanLine(y));
            uchar *row = reinterpret_cast<uchar *>(raw.data()) + y * st;
            for (int x = 0; x < W; ++x) {
                const QRgb c = s[x];
                const int r = qRed(c), g = qGreen(c), b = qBlue(c), a = qAlpha(c);
                auto put16 = [&](quint16 v, bool be) {
                    if (be) { row[x * 2] = uchar(v >> 8); row[x * 2 + 1] = uchar(v); }
                    else { row[x * 2] = uchar(v); row[x * 2 + 1] = uchar(v >> 8); }
                };
                auto e5 = [](int v) { return (v * 31 + 127) / 255; };
                auto e6 = [](int v) { return (v * 63 + 127) / 255; };
                auto e4 = [](int v) { return (v * 15 + 127) / 255; };
                auto d5 = [](int v) { return (v * 255 + 15) / 31; };
                auto d6 = [](int v) { return (v * 255 + 31) / 63; };
                auto setSub = [&](int bits, int value) {   // sub-byte pixel
                    const int per = 8 / bits, byte = x / per, k = x % per;
                    const int shift = lsb ? k * bits : 8 - bits * (k + 1);
                    row[byte] |= uchar(value << shift);
                };
                switch (CP(pixel)) {
                case CP::RGBA8888: row[x * 4] = uchar(r); row[x * 4 + 1] = uchar(g); row[x * 4 + 2] = uchar(b); row[x * 4 + 3] = uchar(a); dl[x] = c; break;
                case CP::ARGB8888: row[x * 4] = uchar(a); row[x * 4 + 1] = uchar(r); row[x * 4 + 2] = uchar(g); row[x * 4 + 3] = uchar(b); dl[x] = c; break;
                case CP::BGRA8888: row[x * 4] = uchar(b); row[x * 4 + 1] = uchar(g); row[x * 4 + 2] = uchar(r); row[x * 4 + 3] = uchar(a); dl[x] = c; break;
                case CP::RGB888: row[x * 3] = uchar(r); row[x * 3 + 1] = uchar(g); row[x * 3 + 2] = uchar(b); dl[x] = qRgb(r, g, b); break;
                case CP::RGB565LE: case CP::RGB565BE: {
                    const int R = e5(r), G = e6(g), B = e5(b);
                    put16(quint16(R << 11 | G << 5 | B), pixel == int(CP::RGB565BE));
                    dl[x] = qRgb(d5(R), d6(G), d5(B));
                    break;
                }
                case CP::RGB555: case CP::ARGB1555: {
                    const int R = e5(r), G = e5(g), B = e5(b), A = pixel == int(CP::ARGB1555) ? (a >= 128 ? 1 : 0) : 0;
                    put16(quint16(A << 15 | R << 10 | G << 5 | B), false);
                    dl[x] = qRgba(d5(R), d5(G), d5(B), pixel == int(CP::ARGB1555) ? A * 255 : 255);
                    break;
                }
                case CP::RGBA4444: {
                    const int R = e4(r), G = e4(g), B = e4(b), A = e4(a);
                    put16(quint16(R << 12 | G << 8 | B << 4 | A), false);
                    dl[x] = qRgba(R * 17, G * 17, B * 17, A * 17);
                    break;
                }
                case CP::Grey8: { const int l = luma(c); row[x] = uchar(l); dl[x] = qRgb(l, l, l); break; }
                case CP::Grey4: { const int l = (luma(c) * 15 + 127) / 255; setSub(4, l); dl[x] = qRgb(l * 17, l * 17, l * 17); break; }
                case CP::Mono1: {
                    const int lit = mono[y * W + x];
                    setSub(1, invert ? 1 - lit : lit);
                    dl[x] = lit ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
                    break;
                }
                case CP::Indexed8: { const int i = idx[fi][y * W + x]; row[x] = uchar(i); dl[x] = d.palette.value(i); break; }
                case CP::Indexed4: { const int i = idx[fi][y * W + x]; setSub(4, i & 15); dl[x] = d.palette.value(i); break; }
                default: break;
                }
            }
        }
        d.raw << raw;
        d.decoded << dec;
    }
    return true;
}

bool writeCHeader(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err, CHeaderData *info) {
    CHeaderData d;
    if (!encodeCHeader(seq, o, d, err)) return false;
    if (o.chStyle >= 2) {
        const QByteArray &r = d.raw[0];
        d.stored << (o.chStyle == 3 ? headerChars(r) : o.chRle ? cheader::gimpRle(r, cheader::gimpBytesPerPixel(d.pixel)) : r);
        d.dataBytes = d.stored[0].size() + 1;   // the string's sizeof, with the terminating NUL
        d.bin = d.stored[0];
        writeGimp(out, d, o);
        if (info) *info = d;
        return true;
    }
    const bool xbm = o.chStyle == 1;
    const QString id = cheader::identifier(o.chName.isEmpty() ? QStringLiteral("image") : o.chName);
    const QString ID = id.toUpper();
    const int frames = d.raw.size();
    const int wb = xbm ? 0 : cheader::wordBits(d.pixel);
    const bool rle = o.chRle && !xbm;
    const bool words = !xbm && !rle && o.chElement == 1 && wb > 0;
    const bool twoD = !xbm && !rle && frames > 1 && o.chFrameLayout == 1;
    const int unitBytes = words ? wb / 8 : 1;
    // stdint.h types, or with "#include <stdint.h>" off only built-in C89 types (unsigned int: 32 bits on every
    // mainstream 32/64-bit target; 16-bit-int targets need unsigned long for 32-bit words)
    const bool stdTypes = o.chStdint && !xbm;
    auto ty = [&](int bits) {
        if (stdTypes) return QStringLiteral("uint%1_t").arg(bits);
        return bits == 8 ? QStringLiteral("unsigned char") : bits == 16 ? QStringLiteral("unsigned short") : QStringLiteral("unsigned int");
    };
    const QString elemType = xbm ? QStringLiteral("unsigned char") : ty(words ? wb : 8);
    // stored bytes per frame (RLE-packed if requested) and the C element values
    QVector<QByteArray> stored;
    for (const QByteArray &r : d.raw) stored << (rle ? packBits(r) : r);
    auto elements = [&](const QByteArray &b) {
        QVector<quint32> v;
        const uchar *p = reinterpret_cast<const uchar *>(b.constData());
        if (!words) { for (int i = 0; i < b.size(); ++i) v << p[i]; return v; }
        for (int i = 0; i + unitBytes <= b.size(); i += unitBytes) {
            if (wb == 32) v << (quint32(p[i]) << 24 | quint32(p[i + 1]) << 16 | quint32(p[i + 2]) << 8 | p[i + 3]);
            else v << (quint32(p[i + 1]) << 8 | p[i]);   // the stored bytes as a little-endian word (RGB565 BE: byte-swapped value)
        }
        return v;
    };
    const int perLine = qBound(1, o.chPerLine, 256);
    const int hexDigits = words ? wb / 4 : 2;
    const bool strip = o.chStrip;   // "strip spaces/tabs/newlines from arrays": {0xFF,0x00,...} on one line
    auto value = [&](quint32 x) {
        return (o.chHex || xbm) ? QStringLiteral("0x%1").arg(x, hexDigits, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("0X"), QLatin1String("0x"))
                                : (!stdTypes && x > 0x7FFFFFFFu) ? QString::number(x) + QLatin1Char('u') : QString::number(x);   // C89: no "unsigned only in ISO C90" warning
    };
    auto body = [&](const QVector<quint32> &v, const QString &indent) {
        QString s;
        if (strip) {
            for (int i = 0; i < v.size(); ++i) { if (i) s += QLatin1Char(','); s += value(v[i]); }
            return s;
        }
        for (int i = 0; i < v.size(); ++i) {
            if (i % perLine == 0) s += indent;
            s += value(v[i]);
            if (i + 1 < v.size()) s += (i % perLine == perLine - 1) ? QStringLiteral(",\n") : QStringLiteral(", ");
        }
        return s + QLatin1Char('\n');
    };
    // "= {...};" initializer: one line when stripping, else the readable block
    auto init = [&](const QVector<quint32> &v) {
        return strip ? QStringLiteral(" = {") + body(v, QString()) + QStringLiteral("};\n\n")
                     : QStringLiteral(" = {\n") + body(v, QStringLiteral("    ")) + QStringLiteral("};\n\n");
    };
    auto list = [&](const QStringList &l) { return strip ? (QLatin1Char('{') + l.join(QLatin1Char(',')) + QLatin1Char('}')) : (QStringLiteral("{ ") + l.join(QStringLiteral(", ")) + QStringLiteral(" }")); };
    QString attrs;
    if (o.chProgmem && !xbm) attrs += QStringLiteral(" PROGMEM");
    if (o.chAligned) attrs += QStringLiteral(" __attribute__((aligned(4)))");
    if (o.chSection && !o.chSectionName.trimmed().isEmpty())
        attrs += QStringLiteral(" __attribute__((section(\"%1\")))").arg(o.chSectionName.trimmed().remove(QLatin1Char('"')));
    const QString cst = o.chConst ? QStringLiteral("const ") : QString();
    const QString guard = ID + QStringLiteral("_H");
    qint64 dataBytes = 0;
    for (const QByteArray &b : stored) dataBytes += b.size();
    const PixInfo &pi = kPix[d.pixel];

    QString h;
    h += QStringLiteral("/*\n * %1.h - generated by %2 %3\n").arg(id, QStringLiteral(LW_PRODUCT_NAME), QStringLiteral(LW_VERSION));
    h += QStringLiteral(" * Source: %1\n").arg(o.sourceName.isEmpty() ? QStringLiteral("(untitled)") : QString(o.sourceName).replace(QLatin1String("*/"), QLatin1String("* /")));
    h += QStringLiteral(" * Size: %1 x %2 px, %3 frame(s)\n").arg(d.width).arg(d.height).arg(frames);
    if (xbm) h += QStringLiteral(" * Format: XBM-style 1-bit bitmap, LSB first, 1 = %1, rows padded to a byte (%2 bytes)\n").arg(o.chInvert ? QStringLiteral("white") : QStringLiteral("black")).arg(d.stride);
    else {
        h += QStringLiteral(" * Format: %1, %2 bpp (%3), stride %4 bytes\n").arg(QLatin1String(pi.name)).arg(pi.bpp).arg(QLatin1String(pi.layout)).arg(d.stride);
        if (d.pixel == int(CP::Mono1) || d.pixel == int(CP::Grey4) || d.pixel == int(CP::Indexed4))
            h += QStringLiteral(" * Bit order: first pixel in the %1 bits%2\n").arg(o.chLsbFirst ? QStringLiteral("least significant") : QStringLiteral("most significant"))
                     .arg(d.pixel == int(CP::Mono1) ? (o.chInvert ? QStringLiteral(", 1 = black") : QStringLiteral(", 1 = white / lit")) : QString());
        h += QStringLiteral(" * Elements: %1%2\n").arg(elemType, words ? QStringLiteral(" (packed words)") : QString());
    }
    h += QStringLiteral(" * Data: %1 bytes%2\n").arg(dataBytes).arg(rle ? QStringLiteral(" (RLE, %1 bytes unpacked)").arg(qint64(d.stride) * d.height * frames) : QString());
    if (rle) {
        h += QStringLiteral(" *\n * RLE (PackBits): read a control byte n; n < 128: copy the next n + 1 bytes;\n"
                            " * n >= 128: repeat the next byte n - 125 times. Decoder:\n"
                            " *   size_t unpack(const uint8_t *in, size_t len, uint8_t *out) {\n"
                            " *       size_t i = 0, o = 0;\n"
                            " *       while (i < len) {\n"
                            " *           unsigned n = in[i++], k;\n"
                            " *           if (n < 128) { for (k = 0; k <= n; ++k) out[o++] = in[i++]; }\n"
                            " *           else { uint8_t v = in[i++]; for (k = 0; k < n - 125; ++k) out[o++] = v; }\n"
                            " *       }\n"
                            " *       return o;\n"
                            " *   }\n").replace(QLatin1String("uint8_t"), stdTypes ? QStringLiteral("uint8_t") : QStringLiteral("unsigned char"));
    }
    h += QStringLiteral(" */\n#ifndef %1\n#define %1\n\n").arg(guard);
    if (o.chStdint && !xbm) h += QStringLiteral("#include <stdint.h>\n\n");
    if (o.chProgmem && !xbm) h += QStringLiteral("#if defined(__AVR__)\n#include <avr/pgmspace.h>\n#endif\n#ifndef PROGMEM\n#define PROGMEM\n#endif\n\n");
    if (xbm) {
        h += QStringLiteral("#define %1_width %2\n#define %1_height %3\n").arg(id).arg(d.width).arg(d.height);
        if (frames > 1) h += QStringLiteral("#define %1_frame_count %2\n").arg(id).arg(frames);
        h += QLatin1Char('\n');
    } else if (o.chDefines) {
        h += QStringLiteral("#define %1_WIDTH %2\n#define %1_HEIGHT %3\n#define %1_FRAMES %4\n#define %1_BPP %5\n#define %1_STRIDE %6\n#define %1_FRAME_BYTES %7\n")
                 .arg(ID).arg(d.width).arg(d.height).arg(frames).arg(pi.bpp).arg(d.stride).arg(d.stride * d.height);
        if (!d.palette.isEmpty()) h += QStringLiteral("#define %1_PALETTE_SIZE %2\n#define %1_TRANSPARENT_INDEX %3\n").arg(ID).arg(d.palette.size()).arg(d.transparentIndex);
        h += QLatin1Char('\n');
    } else {
        auto sc = [&](const QString &n, qint64 v) { h += QStringLiteral("static const unsigned %1_%2 = %3;\n").arg(ID, n).arg(v); };
        sc(QStringLiteral("WIDTH"), d.width); sc(QStringLiteral("HEIGHT"), d.height); sc(QStringLiteral("FRAMES"), frames);
        sc(QStringLiteral("BPP"), pi.bpp); sc(QStringLiteral("STRIDE"), d.stride); sc(QStringLiteral("FRAME_BYTES"), qint64(d.stride) * d.height);
        if (!d.palette.isEmpty()) { sc(QStringLiteral("PALETTE_SIZE"), d.palette.size()); h += QStringLiteral("static const int %1_TRANSPARENT_INDEX = %2;\n").arg(ID).arg(d.transparentIndex); }
        h += QLatin1Char('\n');
    }
    const QString stat = QStringLiteral("static ") + cst;
    if (!d.palette.isEmpty()) {
        QVector<quint32> pv;
        for (QRgb c : d.palette) pv << quint32(qRed(c)) << quint32(qGreen(c)) << quint32(qBlue(c)) << quint32(qAlpha(c));
        QString pb;
        for (int i = 0; i < pv.size(); ++i) {
            if (!strip && i % 16 == 0) pb += QStringLiteral("    ");
            pb += o.chHex ? QStringLiteral("0x%1").arg(pv[i], 2, 16, QLatin1Char('0')).toUpper().replace(QLatin1String("0X"), QLatin1String("0x")) : QString::number(pv[i]);
            if (i + 1 < pv.size()) pb += strip ? QStringLiteral(",") : (i % 16 == 15) ? QStringLiteral(",\n") : QStringLiteral(", ");
        }
        h += QStringLiteral("/* palette: %1 entries of R, G, B, A */\n%2%6 %3_palette[%4]%5").arg(d.palette.size()).arg(stat, id).arg(d.palette.size() * 4).arg(attrs, ty(8))
             + (strip ? QStringLiteral(" = {") + pb + QStringLiteral("};\n\n") : QStringLiteral(" = {\n") + pb + QStringLiteral("\n};\n\n"));
    }
    const QString suffix = xbm ? QStringLiteral("_bits") : QStringLiteral("_data");
    if (twoD) {
        const int n = int(elements(stored[0]).size());
        h += QStringLiteral("%1%2 %3%4[%5][%6]%7").arg(stat, elemType, id, suffix).arg(frames).arg(n).arg(attrs);
        if (strip) {
            h += QStringLiteral(" = {");
            for (int f = 0; f < frames; ++f) h += (f ? QStringLiteral(",{") : QStringLiteral("{")) + body(elements(stored[f]), QString()) + QLatin1Char('}');
            h += QStringLiteral("};\n\n");
        } else {
            h += QStringLiteral(" = {\n");
            for (int f = 0; f < frames; ++f)
                h += QStringLiteral("    { /* frame %1 */\n").arg(f) + body(elements(stored[f]), QStringLiteral("        ")) + (f + 1 < frames ? QStringLiteral("    },\n") : QStringLiteral("    }\n"));
            h += QStringLiteral("};\n\n");
        }
    } else if (frames == 1) {
        const auto v = elements(stored[0]);
        h += QStringLiteral("%1%2 %3%4[%5]%6").arg(stat, elemType, id, suffix).arg(v.size()).arg(attrs) + init(v);
    } else {
        QStringList names, sizes;
        for (int f = 0; f < frames; ++f) {
            const auto v = elements(stored[f]);
            const QString n = QStringLiteral("%1_frame%2%3").arg(id).arg(f).arg(xbm ? suffix : QString());
            h += QStringLiteral("%1%2 %3[%4]%5").arg(stat, elemType, n).arg(v.size()).arg(attrs) + init(v);
            names << n;
            sizes << QString::number(stored[f].size());
        }
        h += QStringLiteral("%1%2 *%3%4_frames[%5] = %6;\n").arg(stat, elemType, o.chConst ? QStringLiteral("const ") : QString(), id).arg(frames).arg(list(names));
        if (rle) h += QStringLiteral("static const %4 %1_frame_sizes[%2] = %3; /* packed bytes per frame */\n").arg(id).arg(frames).arg(list(sizes), ty(32));
        h += QLatin1Char('\n');
    }
    if (frames > 1 && o.chDelays) {
        QStringList ds;
        for (int ms : d.delays) ds << QString::number(ms);
        h += QStringLiteral("%1%2 %3_delays[%4] = %5; /* ms per frame */\n\n").arg(stat, xbm ? QStringLiteral("unsigned short") : ty(16), id).arg(frames).arg(list(ds));
    }
    h += QStringLiteral("#endif /* %1 */\n").arg(guard);
    out = h.toUtf8();
    if (info) {
        d.stored = stored;
        d.dataBytes = dataBytes;
        // the raw .bin: the array bytes as laid out in memory (little-endian words)
        QByteArray bin;
        for (const QByteArray &b : stored) {
            if (!words) { bin += b; continue; }
            for (quint32 v : elements(b)) for (int k = 0; k < unitBytes; ++k) bin.append(char((v >> (8 * k)) & 0xFF));
        }
        d.bin = bin;
        *info = d;
    }
    return true;
}

} // namespace codec
