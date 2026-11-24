// APNG reader/writer implemented on top of Qt's PNG codec plus manual chunk handling,
// since most distro libpng builds lack the APNG patch.
#include "Codecs.h"
#include <QBuffer>
#include <QImageWriter>
#include <QPainter>
#include <QtEndian>
#include <zlib.h>

namespace codec {

namespace {
const char kSig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};

struct Chunk { QByteArray type; QByteArray data; };

QVector<Chunk> parseChunks(const QByteArray &d, bool *ok) {
    QVector<Chunk> out;
    *ok = false;
    if (d.size() < 8 || memcmp(d.constData(), kSig, 8) != 0) return out;
    int pos = 8;
    while (pos + 12 <= d.size()) {
        quint32 len = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(d.constData() + pos));
        if (len > quint32(d.size() - pos - 12)) return out;
        Chunk c;
        c.type = d.mid(pos + 4, 4);
        c.data = d.mid(pos + 8, int(len));
        out.append(c);
        pos += 12 + int(len);
        if (c.type == "IEND") break;
    }
    *ok = true;
    return out;
}

void putU32(QByteArray &b, quint32 v) { char t[4]; qToBigEndian(v, t); b.append(t, 4); }
void putU16(QByteArray &b, quint16 v) { char t[2]; qToBigEndian(v, t); b.append(t, 2); }

void writeChunk(QByteArray &out, const char *type, const QByteArray &data) {
    putU32(out, quint32(data.size()));
    QByteArray td = QByteArray(type, 4) + data;
    out.append(td);
    putU32(out, quint32(crc32(0, reinterpret_cast<const Bytef *>(td.constData()), uInt(td.size()))));
}

quint32 u32(const QByteArray &d, int off)
{
    if (off < 0 || off + 4 > d.size()) return 0;
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(d.constData() + off));
}
quint16 u16(const QByteArray &d, int off)
{
    if (off < 0 || off + 2 > d.size()) return 0;
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(d.constData() + off));
}
}

bool isApng(const QByteArray &d) {
    int pos = 8;
    while (pos + 12 <= d.size()) {
        quint32 len = u32(d, pos);
        QByteArray type = d.mid(pos + 4, 4);
        if (type == "acTL") return true;
        if (type == "IDAT" || type == "IEND") return false;
        if (len > quint32(d.size() - pos - 12)) return false;
        pos += 12 + int(len);
    }
    return false;
}

bool readApng(const QByteArray &d, ImageSeq &out, QString *err) {
    bool ok;
    QVector<Chunk> chunks = parseChunks(d, &ok);
    if (!ok || chunks.isEmpty() || chunks[0].type != "IHDR") { setErr(err, QStringLiteral("APNG: invalid PNG")); return false; }
    const QByteArray ihdr = chunks[0].data;
    if (ihdr.size() < 13) { setErr(err, QStringLiteral("APNG: truncated IHDR")); return false; }
    const int W = int(u32(ihdr, 0)), H = int(u32(ihdr, 4));
    if (W < 1 || H < 1 || W > 65535 || H > 65535) { setErr(err, QStringLiteral("APNG: invalid size")); return false; }
    QVector<Chunk> shared; // PLTE, tRNS, gAMA, ... needed to decode every frame
    struct Fr { int w, h, x, y, delay, dispose, blend; QByteArray data; };
    QVector<Fr> frames;
    bool sawIdat = false, idatIsFrame = false;
    int plays = 0;
    for (const Chunk &c : chunks) {
        if (c.type == "IHDR" || c.type == "IEND") continue;
        if (c.type == "acTL") { if (c.data.size() >= 8) plays = int(u32(c.data, 4)); continue; }
        if (c.type == "fcTL") {
            if (c.data.size() < 26) continue;
            Fr f;
            f.w = int(u32(c.data, 4)); f.h = int(u32(c.data, 8));
            f.x = int(u32(c.data, 12)); f.y = int(u32(c.data, 16));
            int num = u16(c.data, 20), den = u16(c.data, 22);
            if (den == 0) den = 100;
            f.delay = num * 1000 / den;
            f.dispose = uchar(c.data[24]);
            f.blend = uchar(c.data[25]);
            if (!sawIdat) idatIsFrame = true;
            frames.append(f);
            continue;
        }
        if (c.type == "IDAT") {
            sawIdat = true;
            if (idatIsFrame && !frames.isEmpty()) frames.last().data += c.data;
            continue;
        }
        if (c.type == "fdAT") { if (!frames.isEmpty() && c.data.size() > 4) frames.last().data += c.data.mid(4); continue; }
        if (!sawIdat) shared.append(c);
    }
    if (frames.isEmpty()) { setErr(err, QStringLiteral("APNG: no frames")); return false; }
    QImage canvas(W, H, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(0);
    for (const Fr &f : frames) {
        QByteArray png(kSig, 8);
        QByteArray h = ihdr;
        qToBigEndian(quint32(f.w), h.data());
        qToBigEndian(quint32(f.h), h.data() + 4);
        writeChunk(png, "IHDR", h);
        for (const Chunk &c : shared) writeChunk(png, c.type.constData(), c.data);
        writeChunk(png, "IDAT", f.data);
        writeChunk(png, "IEND", {});
        QImage sub = QImage::fromData(png, "PNG");
        if (sub.isNull()) { setErr(err, QStringLiteral("APNG: frame decode failed")); return false; }
        QImage before = f.dispose == 2 ? canvas.copy() : QImage();
        {
            QPainter p(&canvas);
            if (f.blend == 0) p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(f.x, f.y, sub);
        }
        out.frames.append({canvas.convertToFormat(QImage::Format_ARGB32), f.delay > 0 ? f.delay : 100});
        if (f.dispose == 1) {
            QPainter p(&canvas);
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.fillRect(QRect(f.x, f.y, f.w, f.h), Qt::transparent);
        } else if (f.dispose == 2) {
            canvas = before;
        }
    }
    out.loops = plays;
    out.width = W;
    out.height = H;
    return true;
}

bool writeApng(QByteArray &out, const ImageSeq &seq, const ExportOptions &, QString *err) {
    QByteArray ihdr;
    QVector<QByteArray> data;
    for (const auto &f : seq.frames) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        QImageWriter w(&buf, "png");
        if (!w.write(f.image.convertToFormat(QImage::Format_ARGB32))) { setErr(err, w.errorString()); return false; }
        bool ok;
        QVector<Chunk> chunks = parseChunks(png, &ok);
        if (!ok || chunks.isEmpty()) { setErr(err, QStringLiteral("APNG: internal PNG error")); return false; }
        QByteArray idat;
        for (const Chunk &c : chunks) {
            if (c.type == "IHDR") {
                if (ihdr.isEmpty()) ihdr = c.data;
                else if (c.data != ihdr) { setErr(err, QStringLiteral("APNG: frames differ in format")); return false; }
            } else if (c.type == "IDAT") idat += c.data;
        }
        data.append(idat);
    }
    out = QByteArray(kSig, 8);
    writeChunk(out, "IHDR", ihdr);
    QByteArray actl;
    putU32(actl, quint32(seq.frames.size()));
    putU32(actl, quint32(qMax(0, seq.loops)));
    writeChunk(out, "acTL", actl);
    quint32 seqNo = 0;
    const quint32 W = u32(ihdr, 0), H = u32(ihdr, 4);
    for (int i = 0; i < seq.frames.size(); ++i) {
        QByteArray fc;
        putU32(fc, seqNo++);
        putU32(fc, W); putU32(fc, H); putU32(fc, 0); putU32(fc, 0);
        int delay = qBound(1, seq.frames[i].delayMs, 65535);
        putU16(fc, quint16(delay)); putU16(fc, 1000);
        fc.append(char(0)); // dispose: none (frames are full-canvas)
        fc.append(char(0)); // blend: source
        writeChunk(out, "fcTL", fc);
        const QByteArray &d = data[i];
        const int maxChunk = 1 << 20;
        for (int off = 0; off < d.size(); off += maxChunk) {
            QByteArray part = d.mid(off, maxChunk);
            if (i == 0) writeChunk(out, "IDAT", part);
            else { QByteArray fd; putU32(fd, seqNo++); fd += part; writeChunk(out, "fdAT", fd); }
        }
    }
    writeChunk(out, "IEND", {});
    return true;
}

} // namespace codec
