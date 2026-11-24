#include "Codecs.h"
#include "Quantize.h"
#include <gif_lib.h>
#include <cstring>

namespace codec {

namespace {
struct ReadCtx { const QByteArray *d; int pos; };
int gifRead(GifFileType *g, GifByteType *buf, int len) {
    auto *c = static_cast<ReadCtx *>(g->UserData);
    int n = qMin(len, int(c->d->size()) - c->pos);
    if (n <= 0) return 0;
    memcpy(buf, c->d->constData() + c->pos, n);
    c->pos += n;
    return n;
}
int gifWrite(GifFileType *g, const GifByteType *buf, int len) {
    static_cast<QByteArray *>(g->UserData)->append(reinterpret_cast<const char *>(buf), len);
    return len;
}
}

bool readGif(const QByteArray &d, ImageSeq &out, QString *err) {
    ReadCtx ctx{&d, 0};
    int e = 0;
    GifFileType *g = DGifOpen(&ctx, gifRead, &e);
    if (!g) { setErr(err, QStringLiteral("GIF: %1").arg(GifErrorString(e))); return false; }
    int rc = DGifSlurp(g);
    if (rc != GIF_OK && g->ImageCount == 0) {
        setErr(err, QStringLiteral("GIF: %1").arg(GifErrorString(g->Error)));
        DGifCloseFile(g, &e);
        return false;
    }
    const int W = g->SWidth, H = g->SHeight;
    QImage canvas(W, H, QImage::Format_ARGB32);
    canvas.fill(0);
    out.loops = 0;
    bool loopFound = false;
    for (int i = 0; i < g->ImageCount; ++i) {
        SavedImage *si = &g->SavedImages[i];
        for (int k = 0; k + 1 < si->ExtensionBlockCount; ++k) {
            ExtensionBlock &b = si->ExtensionBlocks[k];
            if (b.Function == APPLICATION_EXT_FUNC_CODE && b.ByteCount == 11 && memcmp(b.Bytes, "NETSCAPE2.0", 11) == 0) {
                ExtensionBlock &n = si->ExtensionBlocks[k + 1];
                if (n.ByteCount >= 3 && n.Bytes[0] == 1) {
                    int loops = n.Bytes[1] | (n.Bytes[2] << 8);
                    out.loops = loops == 0 ? 0 : loops + 1;
                    loopFound = true;
                }
            }
        }
        GraphicsControlBlock gcb;
        gcb.DisposalMode = DISPOSAL_UNSPECIFIED;
        gcb.DelayTime = 0;
        gcb.TransparentColor = NO_TRANSPARENT_COLOR;
        gcb.UserInputFlag = false;
        DGifSavedExtensionToGCB(g, i, &gcb);
        ColorMapObject *cm = si->ImageDesc.ColorMap ? si->ImageDesc.ColorMap : g->SColorMap;
        const int fx = si->ImageDesc.Left, fy = si->ImageDesc.Top, fw = si->ImageDesc.Width, fh = si->ImageDesc.Height;
        QImage prev;
        if (gcb.DisposalMode == DISPOSE_PREVIOUS) prev = canvas.copy();
        if (cm && si->RasterBits) {
            for (int y = 0; y < fh; ++y) {
                int cy = fy + y;
                if (cy < 0 || cy >= H) continue;
                QRgb *row = reinterpret_cast<QRgb *>(canvas.scanLine(cy));
                for (int x = 0; x < fw; ++x) {
                    int cx = fx + x;
                    if (cx < 0 || cx >= W) continue;
                    int idx = si->RasterBits[y * fw + x];
                    if (idx == gcb.TransparentColor || idx >= cm->ColorCount) continue;
                    GifColorType c = cm->Colors[idx];
                    row[cx] = qRgba(c.Red, c.Green, c.Blue, 255);
                }
            }
        }
        int delay = gcb.DelayTime * 10;
        if (delay <= 10) delay = 100; // browsers treat 0/1 as 100ms
        out.frames.append({canvas.copy(), delay});
        if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
            for (int y = qMax(0, fy); y < qMin(H, fy + fh); ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(canvas.scanLine(y));
                for (int x = qMax(0, fx); x < qMin(W, fx + fw); ++x) row[x] = 0;
            }
        } else if (gcb.DisposalMode == DISPOSE_PREVIOUS && !prev.isNull()) {
            canvas = prev;
        }
    }
    if (!loopFound && out.frames.size() > 1) out.loops = 1;
    DGifCloseFile(g, &e);
    out.width = W;
    out.height = H;
    return !out.frames.isEmpty();
}

bool writeGif(QByteArray &out, const ImageSeq &seq, const ExportOptions &o, QString *err) {
    const int W = seq.frames[0].image.width(), H = seq.frames[0].image.height();
    bool anyTransparent = false;
    QVector<QImage> imgs;
    for (auto &f : seq.frames) {
        QImage im = f.image.convertToFormat(QImage::Format_ARGB32);
        if (im.size() != QSize(W, H)) im = im.scaled(W, H);
        for (int y = 0; y < H && !anyTransparent; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
            for (int x = 0; x < W; ++x) if (qAlpha(s[x]) < 128) { anyTransparent = true; break; }
        }
        imgs << im;
    }
    int maxColors = qBound(2, o.colors, 256);
    if (anyTransparent && maxColors == 256) maxColors = 255;
    // A single palette across all frames keeps animations stable (as Fireworks' adaptive palette does).
    QVector<QRgb> pal = quant::buildPalette(imgs, maxColors, o.palette, o.customPalette);
    if (anyTransparent && pal.size() > 255) pal.resize(255);
    int offset = anyTransparent ? 1 : 0;
    int total = pal.size() + offset;
    int bits = 1;
    while ((1 << bits) < total) ++bits;
    QVector<GifColorType> colors(1 << bits);
    for (auto &c : colors) c = GifColorType{0, 0, 0};
    for (int i = 0; i < pal.size(); ++i)
        colors[i + offset] = GifColorType{GifByteType(qRed(pal[i])), GifByteType(qGreen(pal[i])), GifByteType(qBlue(pal[i]))};

    int e = 0;
    GifFileType *g = EGifOpen(&out, gifWrite, &e);
    if (!g) { setErr(err, QStringLiteral("GIF: %1").arg(GifErrorString(e))); return false; }
    EGifSetGifVersion(g, true);
    ColorMapObject *global = GifMakeMapObject(1 << bits, colors.data());
    bool ok = EGifPutScreenDesc(g, W, H, bits, 0, global) == GIF_OK;
    if (ok && seq.frames.size() > 1 && seq.loops != 1) {
        unsigned char loopData[3] = {1, 0, 0};
        int loops = seq.loops <= 0 ? 0 : seq.loops - 1;
        loopData[1] = loops & 0xFF;
        loopData[2] = (loops >> 8) & 0xFF;
        ok = EGifPutExtensionLeader(g, APPLICATION_EXT_FUNC_CODE) == GIF_OK &&
             EGifPutExtensionBlock(g, 11, "NETSCAPE2.0") == GIF_OK &&
             EGifPutExtensionBlock(g, 3, loopData) == GIF_OK &&
             EGifPutExtensionTrailer(g) == GIF_OK;
    }
    for (int i = 0; ok && i < imgs.size(); ++i) {
        GraphicsControlBlock gcb;
        gcb.DisposalMode = seq.frames.size() > 1 ? DISPOSE_BACKGROUND : DISPOSAL_UNSPECIFIED;
        gcb.UserInputFlag = false;
        gcb.DelayTime = qMax(2, seq.frames[i].delayMs / 10);
        gcb.TransparentColor = anyTransparent ? 0 : NO_TRANSPARENT_COLOR;
        GifByteType ext[4];
        size_t n = EGifGCBToExtension(&gcb, ext);
        ok = EGifPutExtension(g, GRAPHICS_EXT_FUNC_CODE, int(n), ext) == GIF_OK;
        if (!ok) break;
        QVector<uchar> idx = quant::mapImage(imgs[i], pal, o.dither ? o.ditherAmount : 0, o.loss, anyTransparent ? 0 : -1, offset);
        ok = EGifPutImageDesc(g, 0, 0, W, H, false, nullptr) == GIF_OK;
        for (int y = 0; ok && y < H; ++y) ok = EGifPutLine(g, idx.data() + y * W, W) == GIF_OK;
    }
    int gerr = g->Error;
    if (EGifCloseFile(g, &e) != GIF_OK) ok = false;
    GifFreeMapObject(global);
    if (!ok) setErr(err, QStringLiteral("GIF write failed: %1").arg(GifErrorString(gerr ? gerr : e)));
    return ok;
}

} // namespace codec
