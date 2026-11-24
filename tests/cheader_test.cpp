// v0.2.6 C Header (.h) export tests (linked into roundtrip_test). Every pixel format and the main option
// variants are written, then compiled with the system C compiler (cc -std=c99 -Wall -Wextra -Werror) into a
// small program that checks the dimensions, the array sizes, the frame delays and every pixel value against
// values computed here from the source image (RLE frames are unpacked with the decoder from the header comment).
#include "io/Codecs.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>
#include <cstdio>
#include <functional>

using Report = std::function<void(const QString &, bool, const QString &)>;

namespace {
using namespace cheader;

constexpr int W = 13, H = 7;   // odd width: sub-byte rows are padded

QImage frameImage(int f) {
    QImage img(W, H, QImage::Format_ARGB32);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int a = x == 0 ? 0 : x < 3 ? 40 + 60 * y : x == 3 ? 200 : 255;   // transparent / translucent columns
            img.setPixel(x, y, qRgba((x * 21 + f * 50) & 255, (y * 37 + x * 3) & 255, (x * y * 11 + 255 - f * 30) & 255, a));
        }
    img.setPixel(12, 6, qRgb(255, 255, 255));
    img.setPixel(11, 6, qRgb(0, 0, 0));
    return img;
}

ImageSeq makeSeq(int frames) {
    ImageSeq s;
    s.width = W; s.height = H;
    for (int f = 0; f < frames; ++f) s.frames.append({frameImage(f), 40 + 30 * f});
    return s;
}

int luma(QRgb c) { return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114 + 500) / 1000; }
int e5(int v) { return (v * 31 + 127) / 255; }
int e6(int v) { return (v * 63 + 127) / 255; }
int e4(int v) { return (v * 15 + 127) / 255; }

struct Case {
    QString label;
    int pixel = RGBA8888;
    int frames = 1;
    std::function<void(ExportOptions &)> tweak;
    bool c89 = false;   // v0.2.7: also compile a byte dump with -std=c89 -pedantic (stdint.h off)
};

bool haveCc(QString *cc) {
    for (const char *n : {"cc", "gcc", "clang"}) {
        const QString p = QStandardPaths::findExecutable(QString::fromLatin1(n));
        if (!p.isEmpty()) { *cc = p; return true; }
    }
    return false;
}

// expected per-pixel fields (the C getter returns the same four values), or false to skip the pixel
bool expected(const ExportOptions &o, const codec::CHeaderData &d, int f, int x, int y, int e[4]) {
    const QRgb c = frameImage(f).pixel(x, y);
    const bool xbm = o.chStyle == 1;
    const int p = xbm ? int(Mono1) : o.chPixel;
    const bool alphaFmt = cheader::hasAlpha(p);
    if (!alphaFmt && qAlpha(c) != 255) return false;   // flattened onto the matte: checked through the decoded image
    const int r = qRed(c), g = qGreen(c), b = qBlue(c), a = qAlpha(c);
    e[0] = e[1] = e[2] = e[3] = 0;
    switch (p) {
    case RGBA8888: case ARGB8888: case BGRA8888: e[0] = r; e[1] = g; e[2] = b; e[3] = a; break;
    case RGB888: e[0] = r; e[1] = g; e[2] = b; e[3] = 255; break;
    case RGB565LE: case RGB565BE: e[0] = e5(r); e[1] = e6(g); e[2] = e5(b); break;
    case RGB555: e[0] = e5(r); e[1] = e5(g); e[2] = e5(b); break;
    case ARGB1555: e[0] = e5(r); e[1] = e5(g); e[2] = e5(b); e[3] = a >= 128 ? 1 : 0; break;
    case RGBA4444: e[0] = e4(r); e[1] = e4(g); e[2] = e4(b); e[3] = e4(a); break;
    case Grey8: e[0] = luma(c); break;
    case Grey4: e[0] = (luma(c) * 15 + 127) / 255; break;
    case Mono1: {
        const bool dithered = !xbm && o.dither && o.ditherAmount > 0;
        const int lit = dithered ? (qRed(d.decoded[f].pixel(x, y)) ? 1 : 0) : (luma(c) >= 128 ? 1 : 0);
        e[0] = xbm ? (o.chInvert ? lit : 1 - lit) : (o.chInvert ? 1 - lit : lit);
        break;
    }
    case Indexed8: case Indexed4: {
        const QRgb q = d.decoded[f].pixel(x, y);
        e[0] = qRed(q); e[1] = qGreen(q); e[2] = qBlue(q); e[3] = qAlpha(q);
        if (qAlpha(c) < 128 && d.transparentIndex >= 0) { e[0] = e[1] = e[2] = e[3] = 0; }
        break;
    }
    }
    return true;
}

QString cProgram(const ExportOptions &o, const codec::CHeaderData &d, const QString &headerName, bool dumpBin) {
    const bool xbm = o.chStyle == 1;
    const int p = xbm ? int(Mono1) : o.chPixel;
    const int frames = d.raw.size();
    const QString id = cheader::identifier(o.chName), ID = id.toUpper();
    const bool rle = o.chRle && !xbm;
    const int wb = xbm ? 0 : cheader::wordBits(p);
    const bool words = !xbm && !rle && o.chElement == 1 && wb > 0;
    const bool twoD = !xbm && !rle && frames > 1 && o.chFrameLayout == 1;
    const int fb = d.stride * H, bpp = cheader::bitsPerPixel(p);
    const QString data = id + (xbm ? QStringLiteral("_bits") : QStringLiteral("_data"));
    QString c;
    c += QStringLiteral("#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n#include \"%1\"\n\n").arg(headerName);
    c += QStringLiteral("static int fails = 0;\n#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 12) { printf(__VA_ARGS__); printf(\"\\n\"); } } } while (0)\n");
    c += QStringLiteral("#define NF %1\n#define FB %2\n#define ST %3\n\n").arg(frames).arg(fb).arg(d.stride);
    if (rle) {
        c += QStringLiteral("static uint8_t unp[NF][FB + 256];\n"
                            "static size_t unpack(const uint8_t *in, size_t len, uint8_t *out) {\n"
                            "    size_t i = 0, o = 0;\n"
                            "    while (i < len) {\n"
                            "        unsigned n = in[i++];\n"
                            "        if (n < 128) { for (unsigned k = 0; k <= n; ++k) out[o++] = in[i++]; }\n"
                            "        else { uint8_t v = in[i++]; for (unsigned k = 0; k < n - 125; ++k) out[o++] = v; }\n"
                            "    }\n"
                            "    return o;\n}\n");
    }
    QString elem;
    if (rle) elem = QStringLiteral("unp[f][i]");
    else if (twoD) elem = data + QStringLiteral("[f][i]");
    else if (frames > 1) elem = id + QStringLiteral("_frames[f][i]");
    else elem = data + QStringLiteral("[i]");
    c += QStringLiteral("static unsigned elem(int f, size_t i) { (void)f; return (unsigned)%1; }\n").arg(elem);
    if (words && wb == 16) c += QStringLiteral("static unsigned byteAt(int f, size_t o) { return (elem(f, o / 2) >> (8 * (o % 2))) & 255u; }\n");
    else if (words && wb == 32) c += QStringLiteral("static unsigned byteAt(int f, size_t o) { return (elem(f, o / 4) >> (8 * (3 - o % 4))) & 255u; }\n");
    else c += QStringLiteral("static unsigned byteAt(int f, size_t o) { return elem(f, o) & 255u; }\n");
    c += QStringLiteral("static void get(int f, int x, int y, unsigned v[4]) {\n    v[0] = v[1] = v[2] = v[3] = 0;\n");
    if (bpp >= 8) {
        c += QStringLiteral("    size_t o = (size_t)y * ST + (size_t)x * %1;\n").arg(bpp / 8);
        c += QStringLiteral("    unsigned b0 = byteAt(f, o), b1 = %1, b2 = %2, b3 = %3;\n")
                 .arg(bpp >= 16 ? QStringLiteral("byteAt(f, o + 1)") : QStringLiteral("0"))
                 .arg(bpp >= 24 ? QStringLiteral("byteAt(f, o + 2)") : QStringLiteral("0"))
                 .arg(bpp >= 32 ? QStringLiteral("byteAt(f, o + 3)") : QStringLiteral("0"));
        c += QStringLiteral("    unsigned w = %1;\n    (void)w; (void)b1; (void)b2; (void)b3;\n")
                 .arg(p == RGB565BE ? QStringLiteral("b0 << 8 | b1") : QStringLiteral("b0 | b1 << 8"));
        switch (p) {
        case RGBA8888: c += QStringLiteral("    v[0] = b0; v[1] = b1; v[2] = b2; v[3] = b3;\n"); break;
        case ARGB8888: c += QStringLiteral("    v[0] = b1; v[1] = b2; v[2] = b3; v[3] = b0;\n"); break;
        case BGRA8888: c += QStringLiteral("    v[0] = b2; v[1] = b1; v[2] = b0; v[3] = b3;\n"); break;
        case RGB888: c += QStringLiteral("    v[0] = b0; v[1] = b1; v[2] = b2; v[3] = 255;\n"); break;
        case RGB565LE: case RGB565BE: c += QStringLiteral("    v[0] = w >> 11; v[1] = (w >> 5) & 63; v[2] = w & 31;\n"); break;
        case RGB555: case ARGB1555: c += QStringLiteral("    v[0] = (w >> 10) & 31; v[1] = (w >> 5) & 31; v[2] = w & 31; v[3] = w >> 15;\n"); break;
        case RGBA4444: c += QStringLiteral("    v[0] = w >> 12; v[1] = (w >> 8) & 15; v[2] = (w >> 4) & 15; v[3] = w & 15;\n"); break;
        case Grey8: c += QStringLiteral("    v[0] = b0;\n"); break;
        case Indexed8: c += QStringLiteral("    unsigned i = b0;\n"); break;
        }
    } else {
        const bool lsb = xbm || o.chLsbFirst;
        c += QStringLiteral("    const int per = %1, k = x %% per;\n").arg(8 / bpp).replace(QLatin1String("%%"), QLatin1String("%"));
        c += QStringLiteral("    unsigned byte = byteAt(f, (size_t)y * ST + (size_t)(x / per));\n");
        c += QStringLiteral("    int shift = %1;\n").arg(lsb ? QStringLiteral("k * %1").arg(bpp) : QStringLiteral("8 - %1 * (k + 1)").arg(bpp));
        c += QStringLiteral("    unsigned val = (byte >> shift) & %1u;\n").arg((1 << bpp) - 1);
        if (p == Indexed4) c += QStringLiteral("    unsigned i = val;\n");
        else c += QStringLiteral("    v[0] = val;\n");
    }
    if (cheader::isIndexed(p)) {
        c += QStringLiteral("    CHECK(i < %1_PALETTE_SIZE, \"index %u out of range\", i);\n").arg(ID);
        c += QStringLiteral("    v[0] = %1_palette[i * 4]; v[1] = %1_palette[i * 4 + 1]; v[2] = %1_palette[i * 4 + 2]; v[3] = %1_palette[i * 4 + 3];\n").arg(id);
    }
    c += QStringLiteral("}\n\n");
    // expected values
    QStringList rows;
    for (int f = 0; f < frames; ++f)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int e[4];
                if (expected(o, d, f, x, y, e)) rows << QStringLiteral("{%1,%2,%3,%4,%5,%6,%7}").arg(f).arg(x).arg(y).arg(e[0]).arg(e[1]).arg(e[2]).arg(e[3]);
            }
    c += QStringLiteral("static const int expv[][7] = {\n    %1\n};\n\n").arg(rows.join(QStringLiteral(",\n    ")));
    c += QStringLiteral("int main(int argc, char **argv) {\n    (void)argc; (void)argv;\n");
    if (xbm) {
        c += QStringLiteral("    CHECK(%1_width == %2 && %1_height == %3, \"xbm size\");\n").arg(id).arg(W).arg(H);
        if (frames > 1) c += QStringLiteral("    CHECK(%1_frame_count == NF, \"xbm frames\");\n").arg(id);
    } else {
        c += QStringLiteral("    CHECK(%1_WIDTH == %2, \"width %u\", (unsigned)%1_WIDTH);\n    CHECK(%1_HEIGHT == %3, \"height\");\n"
                            "    CHECK(%1_FRAMES == NF, \"frames\");\n    CHECK(%1_BPP == %4, \"bpp\");\n    CHECK(%1_STRIDE == ST, \"stride\");\n"
                            "    CHECK(%1_FRAME_BYTES == FB, \"frame bytes\");\n").arg(ID).arg(W).arg(H).arg(bpp);
        if (cheader::isIndexed(p))
            c += QStringLiteral("    CHECK(%1_PALETTE_SIZE == %2 && sizeof(%3_palette) == %2 * 4, \"palette size\");\n    CHECK(%1_TRANSPARENT_INDEX == %4, \"transparent index\");\n")
                     .arg(ID).arg(d.palette.size()).arg(id).arg(d.transparentIndex);
    }
    if (rle) {
        if (frames == 1) c += QStringLiteral("    CHECK(unpack(%1, sizeof(%1), unp[0]) == FB, \"rle size\");\n").arg(data);
        else c += QStringLiteral("    for (int f = 0; f < NF; ++f) CHECK(unpack(%1_frames[f], %1_frame_sizes[f], unp[f]) == FB, \"rle size frame %d\", f);\n").arg(id);
    } else if (twoD) c += QStringLiteral("    CHECK(sizeof(%1) == (size_t)NF * FB, \"2D array size %u\", (unsigned)sizeof(%1));\n").arg(data);
    else if (frames > 1) c += QStringLiteral("    CHECK(sizeof(%1_frame0%2) == FB && sizeof(%1_frames) / sizeof(%1_frames[0]) == NF, \"frame array size\");\n").arg(id, xbm ? QStringLiteral("_bits") : QString());
    else c += QStringLiteral("    CHECK(sizeof(%1) == FB, \"array size %u\", (unsigned)sizeof(%1));\n").arg(data);
    if (frames > 1 && o.chDelays)
        for (int f = 0; f < frames; ++f) c += QStringLiteral("    CHECK(%1_delays[%2] == %3, \"delay %2\");\n").arg(id).arg(f).arg(40 + 30 * f);
    c += QStringLiteral("    int n = 0;\n    for (size_t k = 0; k < sizeof(expv) / sizeof(expv[0]); ++k) {\n"
                        "        unsigned v[4];\n        get(expv[k][0], expv[k][1], expv[k][2], v);\n"
                        "        for (int j = 0; j < 4; ++j) CHECK(v[j] == (unsigned)expv[k][3 + j], \"frame %d (%d,%d) field %d: got %u want %d\", expv[k][0], expv[k][1], expv[k][2], j, v[j], expv[k][3 + j]);\n"
                        "        ++n;\n    }\n");
    if (dumpBin)
        c += QStringLiteral("    if (argc > 1) { FILE *fp = fopen(argv[1], \"wb\"); if (fp) { fwrite(%1, 1, sizeof(%1), fp); fclose(fp); } }\n").arg(data);
    c += QStringLiteral("    printf(\"%d pixels checked, %d failures\\n\", n, fails);\n    return fails ? 1 : 0;\n}\n");
    return c;
}

// v0.2.7: a strict C89 program that writes the arrays' memory, frame after frame (compared with CHeaderData::bin)
QString c89Program(const ExportOptions &o, const codec::CHeaderData &d, const QString &headerName) {
    const bool xbm = o.chStyle == 1, rle = o.chRle && !xbm;
    const int frames = d.raw.size();
    const QString id = cheader::identifier(o.chName);
    const QString data = id + (xbm ? QStringLiteral("_bits") : QStringLiteral("_data"));
    const bool twoD = !xbm && !rle && frames > 1 && o.chFrameLayout == 1;
    QString c = QStringLiteral("#include <stdio.h>\n#include \"%1\"\n\nint main(int argc, char **argv) {\n    FILE *fp;\n    unsigned f;\n"
                               "    if (argc < 2) return 2;\n    fp = fopen(argv[1], \"wb\");\n    if (!fp) return 3;\n    f = 0;\n").arg(headerName);
    if (frames == 1 || twoD) c += QStringLiteral("    fwrite(%1, 1, sizeof(%1), fp);\n").arg(data);
    else if (rle) c += QStringLiteral("    for (f = 0; f < %1; ++f) fwrite(%2_frames[f], 1, %2_frame_sizes[f], fp);\n").arg(frames).arg(id);
    else c += QStringLiteral("    for (f = 0; f < %1; ++f) fwrite(%2_frames[f], 1, sizeof(%2_frame0%3), fp);\n").arg(frames).arg(id, xbm ? QStringLiteral("_bits") : QString());
    c += QStringLiteral("    fclose(fp);\n    (void)f;\n    return 0;\n}\n");
    return c;
}

// ---- v0.2.7 GIMP-style output
// byte patterns that stress the string escaping: trigraphs (??/ ??= ???), backslash, quote, digits after short
// octal escapes, space, DEL, high bytes, newline / CR
const uchar kTricky[] = {0, '1', '?', '?', '/', '?', '?', '=', '?', '?', '?', '(', '\\', '"', 32, '7', 127, 255, 8, '0', '9', 10, 13,
                         1, '2', 7, '7', 64, '8', 63, '?', 9, 'a', '\\', '\\', '"', '"', 0, 0, '0', 200, '5', '?', '?', '-', 'Z', 33, 126};
QImage gimpImage(int kind) {
    if (kind == 0) return frameImage(0);
    if (kind == 1 || kind == 2) {   // tricky bytes; 2 = opaque (RGB / RGB565 keep the bytes)
        QImage img(W, H, QImage::Format_ARGB32);
        int k = 0;
        const int n = int(sizeof(kTricky));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int r = kTricky[k++ % n], g = kTricky[k++ % n], b = kTricky[k++ % n];
                const int a = kind == 2 ? 255 : kTricky[k++ % n];
                img.setPixel(x, y, qRgba(r, g, b, a));
            }
        return img;
    }
    if (kind == 3) {   // runs of 1, 2, 127, 128, 200 and a literal stretch longer than 127
        QImage img(300, 3, QImage::Format_ARGB32);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 300; ++x) {
                QRgb c;
                if (y == 0) c = x < 1 ? qRgb(1, 2, 3) : x < 3 ? qRgb(9, 9, 9) : x < 130 ? qRgb(200, 100, 50) : x < 258 ? qRgba(0, 0, 0, 0) : qRgb(5, 6, 7);
                else if (y == 1) c = qRgba((x * 7) & 255, (x * 13) & 255, x & 255, 255 - (x & 1));
                else c = x < 200 ? qRgb('?', '?', '/') : qRgb(255, 255, 255);
                img.setPixel(x, y, c);
            }
        return img;
    }
    if (kind == 4) { QImage img(1, 1, QImage::Format_ARGB32); img.setPixel(0, 0, qRgba('?', '"', '\\', 128)); return img; }
    // 5: GIMP header characters: pixels whose 6-bit groups give "??/", "??=", '"' and '\\'
    QImage img(W, H, QImage::Format_ARGB32);
    const int groups[][4] = {{30, 30, 14, 30}, {30, 28, 30, 30}, {1, 59, 30, 30}, {59, 1, 0, 63}, {30, 30, 30, 30}, {14, 30, 30, 59}};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int *v = groups[(y * W + x) % 6];
            const int d0 = (v[0] << 2) | (v[1] >> 4), d1 = ((v[1] & 15) << 4) | (v[2] >> 2), d2 = ((v[2] & 3) << 6) | v[3];
            img.setPixel(x, y, qRgb(d0, d1, d2));
        }
    return img;
}

// GIMP's pixel bytes, computed here independently (file-csource.c / file-header.c rules, blended onto the matte)
QByteArray gimpExpected(const QImage &img, int bpp, int opacity, QColor matte) {
    QByteArray b;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (bpp == 4) {
                b.append(char(qRed(c))); b.append(char(qGreen(c))); b.append(char(qBlue(c)));
                b.append(char(int(std::floor(qAlpha(c) * opacity / 100.0 + 0.5))));
                continue;
            }
            const double f = qAlpha(c) * opacity / 25500.0;
            const int r = int(std::floor(0.5 + f * qRed(c) + (1 - f) * matte.red()));
            const int g = int(std::floor(0.5 + f * qGreen(c) + (1 - f) * matte.green()));
            const int bl = int(std::floor(0.5 + f * qBlue(c) + (1 - f) * matte.blue()));
            if (bpp == 3) { b.append(char(r)); b.append(char(g)); b.append(char(bl)); continue; }
            const int v = ((r >> 3) << 11) + ((g >> 2) << 5) + (bl >> 3);
            b.append(char(v & 255)); b.append(char(v >> 8));
        }
    return b;
}

QString cBytes(const QByteArray &b) {
    QStringList l;
    for (char ch : b) l << QString::number(uchar(ch));
    return l.join(QLatin1Char(','));
}

// a C99 program that checks the GIMP header's sizes / comment and decodes its string (RLE macro or HEADER_PIXEL)
QString gimpProgram(const ExportOptions &o, int w, int h, const QByteArray &expect, const QString &headerName) {
    QString c = QStringLiteral("#include <stdio.h>\n#include <string.h>\n");
    if (o.chGimpGlib && o.chStyle == 2) c += QStringLiteral("typedef unsigned char guint8;\ntypedef unsigned int guint;\ntypedef char gchar;\n");
    c += QStringLiteral("#include \"%1\"\n\n").arg(headerName);
    c += QStringLiteral("static const unsigned char expv[] = {%1};\n").arg(cBytes(expect));
    if (o.chGimpComment) c += QStringLiteral("static const unsigned char expc[] = {%1};\n").arg(cBytes(o.chGimpCommentText.toUtf8() + QByteArray(1, '\0')));
    c += QStringLiteral("static unsigned char got[%1 + 16];\nint main(int argc, char **argv) {\n    int fails = 0;\n").arg(expect.size());
    if (o.chStyle == 3) {
        c += QStringLiteral("    char *data = header_data;\n    unsigned i;\n    (void)argc; (void)argv;\n"
                            "    if (width != %1 || height != %2) { printf(\"size\\n\"); ++fails; }\n"
                            "    if (strlen(header_data) != (size_t)%1 * %2 * 4) { printf(\"length %u\\n\", (unsigned)strlen(header_data)); ++fails; }\n"
                            "    for (i = 0; i < width * height; ++i) { unsigned char px[3]; HEADER_PIXEL(data, px); memcpy(got + i * 3, px, 3); }\n").arg(w).arg(h);
    } else {
        const int bpp = cheader::gimpBytesPerPixel(o.chPixel);
        const QString name = cheader::identifier(o.chName), M = name.toUpper();
        const bool rle = o.chRle, mac = o.chGimpMacros;
        const QString arr = mac ? QStringLiteral("%1_%2pixel_data").arg(M, rle ? QStringLiteral("rle_") : QString())
                                : QStringLiteral("%1.%2pixel_data").arg(name, rle ? QStringLiteral("rle_") : QString());
        const QString W_ = mac ? M + "_WIDTH" : name + ".width", H_ = mac ? M + "_HEIGHT" : name + ".height", B_ = mac ? M + "_BYTES_PER_PIXEL" : name + ".bytes_per_pixel";
        c += QStringLiteral("    (void)argc; (void)argv;\n    if (%1 != %2 || %3 != %4 || %5 != %6) { printf(\"dims\\n\"); ++fails; }\n").arg(W_).arg(w).arg(H_).arg(h).arg(B_).arg(bpp);
        if (rle) {
            c += QStringLiteral("    %1_RUN_LENGTH_DECODE(got, %2, %3 * %4, %5);\n").arg(M, arr).arg(w).arg(h).arg(bpp);
            if (mac) c += QStringLiteral("    if ((const void *)%1_RLE_PIXEL_DATA != (const void *)%2) ++fails;\n").arg(M, arr);
        } else {
            c += QStringLiteral("    if (sizeof(%1) != %2 + 1) { printf(\"sizeof %u\\n\", (unsigned)sizeof(%1)); ++fails; }\n").arg(arr).arg(expect.size());
            c += QStringLiteral("    memcpy(got, %1, %2);\n").arg(mac ? M + "_PIXEL_DATA" : arr).arg(expect.size());
        }
        if (o.chGimpComment) {
            const QString cm = mac ? M + "_COMMENT" : name + ".comment";
            if (o.chGimpCommentText.isEmpty()) c += QStringLiteral("    if (%1 != 0) ++fails;\n").arg(cm);
            else c += QStringLiteral("    if (strcmp(%1, (const char *)expc) != 0) { printf(\"comment\\n\"); ++fails; }\n").arg(cm);
        }
    }
    c += QStringLiteral("    if (memcmp(got, expv, sizeof(expv)) != 0) { size_t k; for (k = 0; k < sizeof(expv) && got[k] == expv[k]; ++k) {} printf(\"byte %u: %u != %u\\n\", (unsigned)k, got[k], expv[k]); ++fails; }\n"
                        "    printf(\"%u bytes match\\n\", (unsigned)sizeof(expv));\n    return fails ? 1 : 0;\n}\n");
    return c;
}

QByteArray readAll(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool writeAll(const QString &p, const QByteArray &b) { QFile f(p); return f.open(QIODevice::WriteOnly) && f.write(b) == b.size(); }
}

void runCHeaderTests(const QString &tmpDir, const Report &report) {
    // ---- identifiers
    {
        const QList<QPair<QString, QString>> ids = {{"my-icon 2x", "my_icon_2x"}, {"2x", "img_2x"}, {"int", "int_"}, {"", "image"},
                                                    {"h\u00e9llo.png", "h_llo_png"}, {"__a__", "_a"}, {"Logo", "Logo"}, {"--", "image"}};
        QStringList bad;
        for (const auto &p : ids) if (cheader::identifier(p.first) != p.second) bad << p.first + "->" + cheader::identifier(p.first);
        report("C header: identifier sanitising", bad.isEmpty(), bad.join(", "));
        report("C header: format table", ImageIO::extension(Fmt::CHeader) == "h" && !ImageIO::canRead(Fmt::CHeader) && ImageIO::canAnimate(Fmt::CHeader)
                                               && ImageIO::exportFormats().contains(Fmt::CHeader) && cheader::pixelNames().size() == cheader::PixelCount, QString());
    }
    // ---- PackBits round trip on awkward input
    {
        QByteArray in;
        for (int i = 0; i < 300; ++i) in.append(char(i < 140 ? 7 : (i * 13) & 255));
        in.append(QByteArray(2, 'x')).append(QByteArray(131, 'y')).append('z');
        codec::CHeaderData d; QString err;
        ExportOptions o; o.format = Fmt::CHeader; o.chPixel = cheader::Grey8; o.chRle = true;
        ImageSeq s; s.width = in.size(); s.height = 1;
        QImage im(in.size(), 1, QImage::Format_ARGB32);
        for (int i = 0; i < in.size(); ++i) im.setPixel(i, 0, qRgb(uchar(in[i]), uchar(in[i]), uchar(in[i])));
        s.frames.append({im, 0});
        QByteArray h;
        const bool ok = codec::writeCHeader(h, s, o, &err, &d) && d.stored.size() == 1 && cheader::unpackBits(d.stored[0], -1) == d.raw[0]
                        && d.stored[0].size() < d.raw[0].size();
        report("C header: PackBits round trip", ok, err);
    }

    QString cc;
    const bool canCompile = haveCc(&cc);
    if (!canCompile) std::printf("%-34s SKIP (no C compiler)\n", "C header: compiled checks");

    QList<Case> cases;
    for (int p = 0; p < cheader::PixelCount; ++p)
        cases.append({cheader::pixelNames()[p] + " bytes", p, 1, [](ExportOptions &o) { o.dither = false; }});
    for (int p : {RGBA8888, ARGB8888, BGRA8888, RGB565LE, RGB565BE, RGB555, ARGB1555, RGBA4444})
        cases.append({cheader::pixelNames()[p] + " words", p, 1, [](ExportOptions &o) { o.chElement = 1; }});
    cases.append({"RGB565 decimal, 7/line, static const, non-const", RGB565LE, 1, [](ExportOptions &o) { o.chHex = false; o.chPerLine = 7; o.chDefines = false; o.chConst = false; o.chElement = 1; }});
    cases.append({"Indexed 8 static const", Indexed8, 1, [](ExportOptions &o) { o.chDefines = false; o.colors = 20; }});
    cases.append({"1-bit LSB first", Mono1, 1, [](ExportOptions &o) { o.chLsbFirst = true; o.dither = false; }});
    cases.append({"1-bit inverted", Mono1, 1, [](ExportOptions &o) { o.chInvert = true; o.dither = false; }});
    cases.append({"1-bit dithered", Mono1, 1, [](ExportOptions &o) { o.dither = true; o.ditherAmount = 100; }});
    cases.append({"Grey4 LSB first", Grey4, 1, [](ExportOptions &o) { o.chLsbFirst = true; }});
    cases.append({"Indexed 4 LSB first, 9 colours", Indexed4, 1, [](ExportOptions &o) { o.chLsbFirst = true; o.colors = 9; }});
    cases.append({"Indexed 8 web216, no dither", Indexed8, 1, [](ExportOptions &o) { o.palette = 1; o.dither = false; }});
    cases.append({"RGBA8888 RLE", RGBA8888, 1, [](ExportOptions &o) { o.chRle = true; }});
    cases.append({"Indexed 8 RLE", Indexed8, 1, [](ExportOptions &o) { o.chRle = true; o.colors = 8; }});
    cases.append({"1-bit RLE (words ignored)", Mono1, 1, [](ExportOptions &o) { o.chRle = true; o.chElement = 1; o.dither = false; }});
    cases.append({"RGB565 RLE, 3 frames", RGB565LE, 3, [](ExportOptions &o) { o.chRle = true; }});
    cases.append({"RGBA8888 words, 3 frames + delays", RGBA8888, 3, [](ExportOptions &o) { o.chElement = 1; }});
    cases.append({"RGB565 BE words, 2D, 3 frames", RGB565BE, 3, [](ExportOptions &o) { o.chElement = 1; o.chFrameLayout = 1; }});
    cases.append({"Grey8 2D, 2 frames, no delays", Grey8, 2, [](ExportOptions &o) { o.chFrameLayout = 1; o.chDelays = false; }});
    cases.append({"Indexed 8, 3 frames, shared palette", Indexed8, 3, [](ExportOptions &o) { o.colors = 32; }});
    cases.append({"Indexed 4, 2D, 2 frames", Indexed4, 2, [](ExportOptions &o) { o.chFrameLayout = 1; }});
    cases.append({"PROGMEM + aligned + section, no stdint", RGB565LE, 1, [](ExportOptions &o) {
        o.chProgmem = true; o.chAligned = true; o.chSection = true; o.chSectionName = ".rodata.lwimg"; o.chStdint = false; o.chElement = 1; }});
    cases.append({"PROGMEM 1-bit, 2 frames", Mono1, 2, [](ExportOptions &o) { o.chProgmem = true; o.dither = false; }});
    cases.append({"XBM style", Mono1, 1, [](ExportOptions &o) { o.chStyle = 1; o.dither = false; }});
    cases.append({"XBM style inverted, 2 frames", Mono1, 2, [](ExportOptions &o) { o.chStyle = 1; o.chInvert = true; o.dither = false; }});
    // v0.2.7: readable formatting (strip off) for the main layouts
    cases.append({"readable: RGBA8888 bytes", RGBA8888, 1, [](ExportOptions &o) { o.chStrip = false; }});
    cases.append({"readable: Indexed 8, 3 frames + RLE", Indexed8, 3, [](ExportOptions &o) { o.chStrip = false; o.chRle = true; o.colors = 16; }});
    cases.append({"readable: RGB565 BE words, 2D, 3 frames", RGB565BE, 3, [](ExportOptions &o) { o.chStrip = false; o.chElement = 1; o.chFrameLayout = 1; }});
    cases.append({"readable: XBM, 2 frames", Mono1, 2, [](ExportOptions &o) { o.chStrip = false; o.chStyle = 1; o.dither = false; }});
    cases.append({"strip: Indexed 4, 2D, 2 frames, decimal", Indexed4, 2, [](ExportOptions &o) { o.chStrip = true; o.chFrameLayout = 1; o.chHex = false; }});
    // v0.2.7: stdint.h off: built-in types only, also compiled as C89 (-std=c89 -pedantic)
    cases.append({"no stdint: RGBA8888 bytes", RGBA8888, 1, [](ExportOptions &o) { o.chStdint = false; }, true});
    cases.append({"no stdint: RGBA8888 words decimal", RGBA8888, 1, [](ExportOptions &o) { o.chStdint = false; o.chElement = 1; o.chHex = false; }, true});
    cases.append({"no stdint: RGB565 words readable", RGB565LE, 1, [](ExportOptions &o) { o.chStdint = false; o.chElement = 1; o.chStrip = false; }, true});
    cases.append({"no stdint: RGB565 RLE, 3 frames", RGB565LE, 3, [](ExportOptions &o) { o.chStdint = false; o.chRle = true; }, true});
    cases.append({"no stdint: Indexed 8 palette, static const", Indexed8, 1, [](ExportOptions &o) { o.chStdint = false; o.chDefines = false; o.colors = 12; }, true});
    cases.append({"no stdint: ARGB8888 words, 2D, 3 frames", ARGB8888, 3, [](ExportOptions &o) { o.chStdint = false; o.chElement = 1; o.chFrameLayout = 1; }, true});
    cases.append({"no stdint: Grey8, 2 frames readable", Grey8, 2, [](ExportOptions &o) { o.chStdint = false; o.chStrip = false; }, true});

    int n = 0, compiled = 0;
    for (const Case &cs : cases) {
        ++n;
        ExportOptions o; o.format = Fmt::CHeader; o.chPixel = cs.pixel; o.animated = cs.frames > 1;
        o.chName = QStringLiteral("t%1 img").arg(n);   // sanitised to t<n>_img
        o.sourceName = QStringLiteral("case%1.lwd").arg(n);
        if (cs.tweak) cs.tweak(o);
        const QString label = QStringLiteral("C header: ") + cs.label;
        QByteArray h; QString err; codec::CHeaderData d;
        if (!codec::writeCHeader(h, makeSeq(cs.frames), o, &err, &d)) { report(label, false, err); continue; }
        const QString text = QString::fromUtf8(h);
        const QString id = cheader::identifier(o.chName), ID = id.toUpper();
        QStringList problems;
        if (id != QStringLiteral("t%1_img").arg(n)) problems << "identifier " + id;
        if (!text.contains("#ifndef " + ID + "_H\n#define " + ID + "_H") || !text.trimmed().endsWith("#endif /* " + ID + "_H */")) problems << "include guard";
        if (!text.contains(QStringLiteral("Source: case%1.lwd").arg(n)) || !text.contains(QStringLiteral("Size: %1 x %2 px, %3 frame(s)").arg(W).arg(H).arg(cs.frames))) problems << "comment header";
        if (o.chStyle == 0 && text.contains("#include <stdint.h>") != o.chStdint) problems << "stdint include";
        if (o.chProgmem && o.chStyle == 0 && !(text.contains("#include <avr/pgmspace.h>") && text.contains("] PROGMEM"))) problems << "PROGMEM";
        if (o.chAligned && !text.contains("__attribute__((aligned(4)))")) problems << "aligned";
        if (o.chSection && !text.contains("__attribute__((section(\".rodata.lwimg\")))")) problems << "section";
        if (o.chRle && o.chStyle == 0 && !text.contains(o.chStdint ? "size_t unpack(const uint8_t *in" : "size_t unpack(const unsigned char *in")) problems << "RLE decoder comment";
        if (!o.chStdint && o.chStyle == 0 && (text.contains("uint8_t") || text.contains("uint16_t") || text.contains("uint32_t") || text.contains("stdint")))
            problems << "stdint types without stdint.h";
        {
            // strip: every initializer on one line with no whitespace in the data; readable: indented lines
            QString code = text;
            code.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption));
            const bool hasStrip = !code.contains(QStringLiteral(", 0x")) && !code.contains(QStringLiteral("= {\n")) && !code.contains(QStringLiteral("\n    "))
                                  && !code.contains(QStringLiteral("= { ")) && !QRegularExpression(QStringLiteral("\\d, \\d")).match(code).hasMatch();
            if (o.chStrip && !hasStrip) { problems << "whitespace in stripped arrays"; if (qEnvironmentVariableIsSet("LW_DUMP")) std::fprintf(stderr, "%s\n", h.constData()); }
            if (!o.chStrip && !text.contains(QStringLiteral("= {\n    "))) problems << "readable layout";
        }
        if (d.raw.size() != cs.frames || d.decoded.size() != cs.frames || d.stride != cheader::stride(o.chStyle ? int(Mono1) : cs.pixel, W)) problems << "frame data";
        if (cheader::isIndexed(cs.pixel) && o.chStyle == 0 && (d.palette.isEmpty() || d.palette.size() > (cs.pixel == Indexed4 ? 16 : qMin(256, o.colors)) || d.transparentIndex != 0)) problems << QStringLiteral("palette %1").arg(d.palette.size());
        if (!canCompile) { report(label, problems.isEmpty(), problems.join(", ")); continue; }
        const QString dir = QDir(tmpDir).filePath(QStringLiteral("cheader_%1").arg(n));
        QDir().mkpath(dir);
        const QString hdr = id + ".h";
        const bool dumpBin = cs.frames == 1 && !(o.chRle && o.chStyle == 0);
        writeAll(QDir(dir).filePath(hdr), h);
        writeAll(QDir(dir).filePath("test.c"), cProgram(o, d, hdr, dumpBin).toUtf8());
        QProcess pc;
        pc.setWorkingDirectory(dir);
        pc.setProcessChannelMode(QProcess::MergedChannels);
        pc.start(cc, {"-std=c99", "-Wall", "-Wextra", "-Werror", "-o", "test", "test.c"});
        if (!pc.waitForFinished(60000) || pc.exitCode() != 0) {
            report(label, false, "cc: " + QString::fromLocal8Bit(pc.readAll()).left(600));
            continue;
        }
        QProcess run;
        run.setWorkingDirectory(dir);
        run.setProcessChannelMode(QProcess::MergedChannels);
        run.start(QDir(dir).filePath("test"), {QDir(dir).filePath("mem.bin")});
        const bool ran = run.waitForFinished(30000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
        const QString out = QString::fromLocal8Bit(run.readAll()).trimmed();
        if (!ran) problems << "run: " + out.left(600);
        if (ran && dumpBin && readAll(QDir(dir).filePath("mem.bin")) != d.bin) problems << ".bin differs from the array's memory";
        if (cs.c89) {
            writeAll(QDir(dir).filePath("dump89.c"), c89Program(o, d, hdr).toUtf8());
            QProcess p89;
            p89.setWorkingDirectory(dir);
            p89.setProcessChannelMode(QProcess::MergedChannels);
            p89.start(cc, {"-std=c89", "-pedantic", "-Wall", "-Wextra", "-Werror", "-o", "dump89", "dump89.c"});
            if (!p89.waitForFinished(60000) || p89.exitCode() != 0) problems << "c89: " + QString::fromLocal8Bit(p89.readAll()).left(600);
            else {
                QProcess r89;
                r89.start(QDir(dir).filePath("dump89"), {QDir(dir).filePath("mem89.bin")});
                if (!r89.waitForFinished(30000) || r89.exitCode() != 0) problems << "c89 run";
                else if (readAll(QDir(dir).filePath("mem89.bin")) != d.bin) problems << "c89 dump differs from the stored bytes";
            }
        }
        if (problems.isEmpty()) ++compiled;
        report(label, problems.isEmpty(), problems.isEmpty() ? QStringLiteral("%1 bytes header, %2 data bytes; %3").arg(h.size()).arg(d.dataBytes).arg(out) : problems.join("; "));
    }
    if (canCompile) report("C header: all formats compile and verify", compiled == cases.size(), QStringLiteral("%1 of %2").arg(compiled).arg(cases.size()));

    // ---- ImageIO::write: identifier from the file name, one frame unless animated, raw .bin alongside
    {
        ImageSeq s = makeSeq(3);
        ExportOptions o; o.format = Fmt::CHeader; o.chPixel = RGB565LE; o.chElement = 1; o.chRawBin = true;
        const QString path = QDir(tmpDir).filePath("my-icon 2x.h");
        QString err;
        const bool ok = ImageIO::write(path, s, o, &err);
        const QString text = QString::fromUtf8(readAll(path));
        QByteArray h; codec::CHeaderData d;
        ExportOptions o2 = o; o2.chName = "my-icon 2x";
        ImageSeq one = s; one.frames.resize(1);
        codec::writeCHeader(h, one, o2, nullptr, &d);
        const QByteArray bin = readAll(QDir(tmpDir).filePath("my-icon 2x.bin"));
        report("C header: write() names, .bin", ok && text.contains("uint16_t my_icon_2x_data[91]") && text.contains("MY_ICON_2X_FRAMES 1")
                                                    && bin == d.bin && bin.size() == W * H * 2, err);
        o.animated = true; o.chRawBin = false; o.chName = "anim";
        const QString p2 = QDir(tmpDir).filePath("anim.h");
        const bool ok2 = ImageIO::write(p2, s, o, &err);
        const QString t2 = QString::fromUtf8(readAll(p2));
        report("C header: write() animated", ok2 && t2.contains("ANIM_FRAMES 3") && t2.contains("anim_frames[3]") && t2.contains("anim_delays[3] = {40,70,100}")
                                                && !QFileInfo::exists(QDir(tmpDir).filePath("anim.bin")), err);
    }
    // ---- LWD JSON round trip of every C header option
    {
        ExportOptions e; e.format = Fmt::CHeader; e.chPixel = Indexed4; e.chElement = 1; e.chHex = false; e.chPerLine = 9; e.chConst = false;
        e.chProgmem = true; e.chAligned = true; e.chSection = true; e.chSectionName = ".flash"; e.chStdint = false; e.chDefines = false;
        e.chFrameLayout = 1; e.chDelays = false; e.chRle = true; e.chStyle = 1; e.chRawBin = true; e.chLsbFirst = true; e.chInvert = true; e.chName = "sprite";
        e.chGimpMacros = true; e.chGimpGlib = true; e.chGimpComment = true; e.chGimpCommentText = "hi ?\?/"; e.chGimpOpacity = 42; e.chStrip = false;
        ExportOptions back = lwd::exportOptionsFromJson(lwd::exportOptionsToJson(e));
        ExportOptions def; def.format = Fmt::CHeader;
        report("C header: options survive .lwd JSON", back == e && !(def == e) && back.chSectionName == ".flash" && back.chName == "sprite", QString());
        ExportOptions g; g.format = Fmt::CHeader; g.chStyle = 3;
        report("C header: GIMP style + strip survive .lwd JSON", lwd::exportOptionsFromJson(lwd::exportOptionsToJson(g)) == g && back.chGimpOpacity == 42 && !back.chStrip, QString());
    }
    // ---- v0.2.7 GIMP RLE codec on its own (incl. the 1-pixel case where GIMP asserts)
    {
        QStringList bad;
        for (int kind : {0, 1, 3, 4})
            for (int bpp : {2, 3, 4}) {
                const QByteArray raw = gimpExpected(gimpImage(kind), bpp, 100, Qt::black);
                const QByteArray enc = cheader::gimpRle(raw, bpp);
                if (cheader::gimpRleDecode(enc, bpp) != raw) bad << QStringLiteral("image %1 bpp %2").arg(kind).arg(bpp);
            }
        const QByteArray one = cheader::gimpRle(QByteArray("\1\2\3", 3), 3);
        const QByteArray run = cheader::gimpRle(QByteArray(300 * 3, 'x'), 3);   // 127 + 127 + 46 repeats
        report("C header: GIMP RLE codec", bad.isEmpty() && one == QByteArray("\x01\x01\x02\x03", 4)
                                               && run.size() == 12 && uchar(run[0]) == (127 | 128) && uchar(run[8]) == (46 | 128), bad.join(", "));
    }
    // ---- v0.2.7 GIMP C source / GIMP header, compiled and decoded byte for byte
    {
        struct G { QString label; int style; int pixel; int image; std::function<void(ExportOptions &)> tweak; };
        QList<G> gs;
        gs.append({"struct RGB", 2, RGB888, 0, nullptr});
        gs.append({"struct RGBA, tricky bytes", 2, RGBA8888, 1, nullptr});
        gs.append({"struct RGBA, tricky, readable", 2, RGBA8888, 1, [](ExportOptions &o) { o.chStrip = false; }});
        gs.append({"macros RGB, tricky, readable", 2, RGB888, 2, [](ExportOptions &o) { o.chGimpMacros = true; o.chStrip = false; }});
        gs.append({"macros RGB, tricky", 2, RGB888, 2, [](ExportOptions &o) { o.chGimpMacros = true; }});
        gs.append({"struct RGB565 RLE, runs", 2, RGB565LE, 3, [](ExportOptions &o) { o.chRle = true; }});
        gs.append({"struct RGB565 BE pick -> RGB565, tricky", 2, RGB565BE, 2, nullptr});
        gs.append({"macros glib RGBA RLE + comment, runs", 2, RGBA8888, 3, [](ExportOptions &o) {
            o.chRle = true; o.chGimpMacros = true; o.chGimpGlib = true; o.chGimpComment = true; o.chGimpCommentText = QStringLiteral("line 1 \"q\" ?\?/\nline\\2 ?\?= \t\x7f end"); }});
        gs.append({"struct glib RGB RLE + comment, readable", 2, RGB888, 3, [](ExportOptions &o) {
            o.chRle = true; o.chGimpGlib = true; o.chGimpComment = true; o.chStrip = false; o.chGimpCommentText = QStringLiteral("multi\nline\n?\?( comment"); }});
        gs.append({"macros glib RGB RLE + comment, readable", 2, RGB888, 3, [](ExportOptions &o) {
            o.chRle = true; o.chGimpGlib = true; o.chGimpMacros = true; o.chGimpComment = true; o.chStrip = false; o.chGimpCommentText = QStringLiteral("a\nb"); }});
        gs.append({"macros RGB565, opacity 60, white matte, readable", 2, RGB565LE, 0, [](ExportOptions &o) { o.chGimpMacros = true; o.chGimpOpacity = 60; o.matte = Qt::white; o.chStrip = false; }});
        gs.append({"struct RGBA opacity 50", 2, RGBA8888, 0, [](ExportOptions &o) { o.chGimpOpacity = 50; }});
        gs.append({"struct RGB, grey matte", 2, RGB888, 0, [](ExportOptions &o) { o.matte = QColor(90, 128, 200); }});
        gs.append({"RLE 1 pixel RGBA", 2, RGBA8888, 4, [](ExportOptions &o) { o.chRle = true; }});
        gs.append({"RLE 1 pixel RGB macros", 2, RGB888, 4, [](ExportOptions &o) { o.chRle = true; o.chGimpMacros = true; }});
        gs.append({"RLE RGBA tricky readable", 2, RGBA8888, 1, [](ExportOptions &o) { o.chRle = true; o.chStrip = false; }});
        gs.append({"empty comment = NULL (struct)", 2, RGB888, 0, [](ExportOptions &o) { o.chGimpComment = true; o.chGimpCommentText.clear(); }});
        gs.append({"empty comment = NULL (macros, glib)", 2, RGB888, 0, [](ExportOptions &o) { o.chGimpComment = true; o.chGimpCommentText.clear(); o.chGimpMacros = true; o.chGimpGlib = true; }});
        gs.append({"header", 3, RGBA8888, 0, nullptr});
        gs.append({"header, trigraph chars", 3, RGB888, 5, nullptr});
        gs.append({"header, trigraph chars, readable", 3, RGB888, 5, [](ExportOptions &o) { o.chStrip = false; }});
        gs.append({"header, 1 pixel", 3, RGB888, 4, nullptr});
        gs.append({"header, opacity 70, readable", 3, RGB565LE, 3, [](ExportOptions &o) { o.chGimpOpacity = 70; o.chStrip = false; }});
        int gn = 0, gok = 0;
        for (const G &g : gs) {
            ++gn;
            ExportOptions o; o.format = Fmt::CHeader; o.chStyle = g.style; o.chPixel = g.pixel; o.matte = Qt::black;
            o.chName = QStringLiteral("g%1 img").arg(gn);
            if (g.tweak) g.tweak(o);
            const QString label = QStringLiteral("C header GIMP: ") + g.label;
            const QImage img = gimpImage(g.image);
            ImageSeq seq; seq.width = img.width(); seq.height = img.height(); seq.frames.append({img, 0});
            seq.frames.append({frameImage(1), 0});   // a second frame is ignored (GIMP writes one image)
            QByteArray h; QString err; codec::CHeaderData d;
            if (!codec::writeCHeader(h, seq, o, &err, &d)) { report(label, false, err); continue; }
            const int bpp = g.style == 3 ? 3 : cheader::gimpBytesPerPixel(g.pixel);
            const QByteArray expect = gimpExpected(img, bpp, o.chGimpOpacity, o.matte);
            const QString text = QString::fromUtf8(h);
            QStringList problems;
            if (d.raw.value(0) != expect) problems << "encoded bytes differ from GIMP's rules";
            if (d.decoded.size() != 1) problems << "frames";
            if (text.contains(QStringLiteral("?\?/")) || text.contains(QStringLiteral("?\?=")) || text.contains(QStringLiteral("?\?("))) problems << "trigraph in output";
            const QStringList lines = text.split(QLatin1Char('\n'));
            int dataLines = 0;
            for (const QString &l : lines) if (l.startsWith(QStringLiteral("  \"")) || l.startsWith(QStringLiteral(" \"")) || l.startsWith(QStringLiteral("(\"")) || l.startsWith(QStringLiteral("\t\"")) || (g.style == 3 && l.startsWith(QStringLiteral("\"")))) ++dataLines;
            const int commentLines = o.chGimpComment && !o.chGimpCommentText.isEmpty() ? (o.chStrip ? 1 : int(o.chGimpCommentText.count(QLatin1Char('\n'))) + 1) : 0;
            if (o.chStrip && (dataLines - commentLines != 1 || text.contains(QStringLiteral("\"\"")))) problems << QStringLiteral("strip: %1 data lines").arg(dataLines);
            if (!o.chStrip && img.width() * img.height() > 32 && dataLines - commentLines < 2) problems << "readable: not wrapped";
            if (g.style == 2 && !text.startsWith(QStringLiteral("/* GIMP %1 C-Source image dump %2(").arg(bpp == 4 ? "RGBA" : "RGB", o.chRle ? "1-byte-run-length-encoded " : ""))) problems << "first line";
            if (g.style == 3 && !text.startsWith(QStringLiteral("/*  GIMP header image file format (RGB): "))) problems << "first line";
            if (!canCompile) { report(label, problems.isEmpty(), problems.join(", ")); continue; }
            const QString dir = QDir(tmpDir).filePath(QStringLiteral("gimp_%1").arg(gn));
            QDir().mkpath(dir);
            writeAll(QDir(dir).filePath("img.h"), h);
            writeAll(QDir(dir).filePath("test.c"), gimpProgram(o, img.width(), img.height(), expect, "img.h").toUtf8());
            QProcess pc;
            pc.setWorkingDirectory(dir);
            pc.setProcessChannelMode(QProcess::MergedChannels);
            pc.start(cc, {"-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-o", "test", "test.c"});
            if (!pc.waitForFinished(60000) || pc.exitCode() != 0) { report(label, false, "cc: " + QString::fromLocal8Bit(pc.readAll()).left(800)); continue; }
            QProcess run;
            run.setProcessChannelMode(QProcess::MergedChannels);
            run.start(QDir(dir).filePath("test"), {});
            const bool ran = run.waitForFinished(30000) && run.exitStatus() == QProcess::NormalExit && run.exitCode() == 0;
            const QString out = QString::fromLocal8Bit(run.readAll()).trimmed();
            if (!ran) problems << "run: " + out.left(400);
            if (problems.isEmpty()) ++gok;
            report(label, problems.isEmpty(), problems.isEmpty() ? QStringLiteral("%1 bytes header; %2").arg(h.size()).arg(out) : problems.join("; "));
        }
        if (canCompile) report("C header GIMP: all compile and decode", gok == gs.size(), QStringLiteral("%1 of %2").arg(gok).arg(gs.size()));
    }
}
