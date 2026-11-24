#include "ExportPresets.h"

namespace exportpresets {
QStringList names() {
    QStringList l;
    for (const auto &p : kPresets) l << QString::fromLatin1(p.name);
    return l;
}
int indexOf(const QString &name) {
    for (int i = 0; i < kCount; ++i) if (QString::fromLatin1(kPresets[i].name).compare(name.trimmed(), Qt::CaseInsensitive) == 0) return i;
    return -1;
}
ExportOptions apply(int i, ExportOptions o) {
    if (i < 0 || i >= kCount) return o;
    const Preset &p = kPresets[i];
    o.format = p.f; o.quality = p.q; o.lossless = p.lossless; o.colors = p.colors; o.palette = p.palette;
    o.dither = p.dither; o.ditherAmount = 100; o.loss = 0; o.animated = p.anim; o.netpbmAscii = p.ascii;
    o.tiffCompression = p.tiffComp; o.tiff16 = p.b16; o.alpha = p.alpha; o.tgaRle = p.rle;
    if (p.f == Fmt::PNG) { o.pngType = p.pngType; o.png16 = false; o.tinyPng = false; o.pngTransparency = 2; o.pngEditable = false; }
    if (p.f == Fmt::CHeader) {
        o.chPixel = p.chPixel; o.chElement = p.chElement; o.chStyle = p.chStyle; o.chRle = p.chStyle == 2 && p.chPixel == cheader::RGB565LE; o.chHex = true;
        o.chLsbFirst = false; o.chInvert = false;
        o.chPerLine = p.chPixel == cheader::RGB565LE ? 12 : 16;
        o.chAligned = p.chPixel == cheader::RGB565LE;
    }
    if (p.f == Fmt::JPEG) {
        o.jpegEncoder = codec::jpegEncoderAvailable(p.jpegEnc) ? p.jpegEnc : 0;
        o.jpegProgressive = p.jpegProg; o.jpegOptimize = p.jpegOpt;
        o.jpegSubsampling = 2; o.jpegSmoothing = 0; o.jpegSharpenEdges = false;
    }
    return o;
}
}
