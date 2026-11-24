#include "Brand.h"
// Netpbm: reads P1-P6 (PBM/PGM/PPM, ASCII and binary, 8/16-bit), P7 PAM (any tuple type, 8/16-bit)
// and PFM (Pf/PF, linear float); writes P1/P4 PBM, P2/P3/P5/P6, P7 PAM (RGB / RGB_ALPHA) and PF.
#include "Codecs.h"
#include "Quantize.h"
#include <cmath>
#include <cstring>

namespace codec {

namespace {
struct Tok {
    const QByteArray &d;
    int pos = 0;
    void skipWs() {
        while (pos < d.size()) {
            char c = d[pos];
            if (c == '#') { while (pos < d.size() && d[pos] != '\n' && d[pos] != '\r') ++pos; }
            else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') ++pos;
            else break;
        }
    }
    bool num(int &v) {
        skipWs();
        if (pos >= d.size() || d[pos] < '0' || d[pos] > '9') return false;
        long long x = 0;
        while (pos < d.size() && d[pos] >= '0' && d[pos] <= '9') { x = x * 10 + (d[pos] - '0'); if (x > 1 << 30) return false; ++pos; }
        v = int(x);
        return true;
    }
    bool bit(int &v) { // P1 allows digits without separators
        skipWs();
        if (pos >= d.size() || (d[pos] != '0' && d[pos] != '1')) return false;
        v = d[pos++] - '0';
        return true;
    }
};
}

bool readNetpbm(const QByteArray &d, ImageSeq &out, QString *err) {
    if (d.size() < 3 || d[0] != 'P' || d[1] < '1' || d[1] > '6') { setErr(err, QStringLiteral("Netpbm: bad magic")); return false; }
    const int kind = d[1] - '0';
    Tok t{d, 2};
    int w, h, maxv = 1;
    if (!t.num(w) || !t.num(h) || w <= 0 || h <= 0 || qint64(w) * h > (1 << 28)) { setErr(err, QStringLiteral("Netpbm: bad header")); return false; }
    if (kind != 1 && kind != 4 && (!t.num(maxv) || maxv <= 0 || maxv > 65535)) { setErr(err, QStringLiteral("Netpbm: bad maxval")); return false; }
    QImage img(w, h, QImage::Format_ARGB32);
    const bool binary = kind >= 4;
    if (binary) t.pos++; // single whitespace after header
    const int ch = (kind == 3 || kind == 6) ? 3 : 1;
    const int bps = maxv > 255 ? 2 : 1;
    auto scale = [&](int v) { return maxv == 255 ? v : qBound(0, (v * 255 + maxv / 2) / maxv, 255); };
    const uchar *raw = reinterpret_cast<const uchar *>(d.constData());
    for (int y = 0; y < h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        if (kind == 4) {
            int rowBytes = (w + 7) / 8;
            if (t.pos + rowBytes > d.size()) { setErr(err, QStringLiteral("Netpbm: truncated data")); return false; }
            for (int x = 0; x < w; ++x) {
                bool black = raw[t.pos + x / 8] & (0x80 >> (x % 8));
                row[x] = black ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
            }
            t.pos += rowBytes;
            continue;
        }
        for (int x = 0; x < w; ++x) {
            int v[3] = {0, 0, 0};
            for (int c = 0; c < ch; ++c) {
                if (kind == 1) { if (!t.bit(v[c])) { setErr(err, QStringLiteral("Netpbm: truncated data")); return false; } }
                else if (!binary) { if (!t.num(v[c])) { setErr(err, QStringLiteral("Netpbm: truncated data")); return false; } }
                else {
                    if (t.pos + bps > d.size()) { setErr(err, QStringLiteral("Netpbm: truncated data")); return false; }
                    v[c] = bps == 2 ? (raw[t.pos] << 8) | raw[t.pos + 1] : raw[t.pos];
                    t.pos += bps;
                }
            }
            if (kind == 1) row[x] = v[0] ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
            else if (ch == 1) { int g = scale(v[0]); row[x] = qRgb(g, g, g); }
            else row[x] = qRgb(scale(v[0]), scale(v[1]), scale(v[2]));
        }
    }
    out.frames = {{img, 100}};
    out.width = w;
    out.height = h;
    out.loops = 0;
    return true;
}

bool writeNetpbm(QByteArray &out, const QImage &src, bool gray, bool ascii, QString *) {
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    const char magic = gray ? (ascii ? '2' : '5') : (ascii ? '3' : '6');
    out = QByteArray("P") + magic + "\n# Created by " LW_PRODUCT_NAME "\n" + QByteArray::number(w) + ' ' + QByteArray::number(h) + "\n255\n";
    if (!ascii) out.reserve(out.size() + w * h * (gray ? 1 : 3));
    QByteArray line;
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            int vals[3] = {qRed(s[x]), qGreen(s[x]), qBlue(s[x])};
            int n = 3;
            if (gray) { vals[0] = qGray(s[x]); n = 1; }
            for (int c = 0; c < n; ++c) {
                if (!ascii) { out.append(char(vals[c])); continue; }
                QByteArray num = QByteArray::number(vals[c]);
                if (line.size() + num.size() + 1 > 70) { out += line + '\n'; line.clear(); }
                if (!line.isEmpty()) line += ' ';
                line += num;
            }
        }
        if (ascii && !line.isEmpty()) { out += line + '\n'; line.clear(); }
    }
    return true;
}


bool writePbm(QByteArray &out, const QImage &src, bool ascii, bool dither, QString *) {
    const QImage img = flatten(src, Qt::white);
    const int w = img.width(), h = img.height();
    const QVector<QRgb> bw = {qRgb(0, 0, 0), qRgb(255, 255, 255)};
    const QVector<uchar> idx = quant::mapImage(img, bw, dither ? 100 : 0, 0, -1, 0);
    out = QByteArray("P") + (ascii ? '1' : '4') + "\n# Created by " LW_PRODUCT_NAME "\n" + QByteArray::number(w) + ' ' + QByteArray::number(h) + "\n";
    for (int y = 0; y < h; ++y) {
        if (ascii) {
            QByteArray line;
            for (int x = 0; x < w; ++x) {
                line += idx[y * w + x] == 0 ? '1' : '0';   // 1 = black
                if (line.size() >= 69) { out += line + '\n'; line.clear(); }
            }
            out += line + '\n';
        } else {
            QByteArray row((w + 7) / 8, 0);
            for (int x = 0; x < w; ++x) if (idx[y * w + x] == 0) row[x / 8] = char(uchar(row[x / 8]) | (0x80 >> (x % 8)));
            out += row;
        }
    }
    return true;
}

bool readPam(const QByteArray &d, ImageSeq &out, QString *err) {
    if (!d.startsWith("P7")) { setErr(err, QStringLiteral("PAM: bad magic")); return false; }
    int pos = 2, w = 0, h = 0, depth = 0, maxv = 0;
    QByteArray tupl;
    bool ended = false;
    while (pos < d.size()) {
        const int eol = d.indexOf('\n', pos);
        if (eol < 0) break;
        const QByteArray line = d.mid(pos, eol - pos).trimmed();
        pos = eol + 1;
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line == "ENDHDR") { ended = true; break; }
        const int sp = line.indexOf(' ');
        const QByteArray key = sp < 0 ? line : line.left(sp), val = sp < 0 ? QByteArray() : line.mid(sp + 1).trimmed();
        if (key == "WIDTH") w = val.toInt();
        else if (key == "HEIGHT") h = val.toInt();
        else if (key == "DEPTH") depth = val.toInt();
        else if (key == "MAXVAL") maxv = val.toInt();
        else if (key == "TUPLTYPE") tupl += (tupl.isEmpty() ? "" : " ") + val;
    }
    if (!ended || w <= 0 || h <= 0 || depth < 1 || depth > 4 || maxv < 1 || maxv > 65535 || qint64(w) * h > (1 << 28)) {
        setErr(err, QStringLiteral("PAM: bad header")); return false;
    }
    const int bps = maxv > 255 ? 2 : 1;
    if (pos + qint64(w) * h * depth * bps > d.size()) { setErr(err, QStringLiteral("PAM: truncated data")); return false; }
    const uchar *p = reinterpret_cast<const uchar *>(d.constData()) + pos;
    const bool bw = tupl.startsWith("BLACKANDWHITE");
    auto sample = [&](qint64 i) { const int v = bps == 2 ? (p[i * 2] << 8 | p[i * 2 + 1]) : p[i]; return bw ? (v ? 255 : 0) : (v * 255 + maxv / 2) / maxv; };
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const qint64 b = (qint64(y) * w + x) * depth;
            switch (depth) {
            case 1: { const int g = sample(b); row[x] = qRgb(g, g, g); break; }
            case 2: { const int g = sample(b); row[x] = qRgba(g, g, g, sample(b + 1)); break; }
            case 3: row[x] = qRgb(sample(b), sample(b + 1), sample(b + 2)); break;
            default: row[x] = qRgba(sample(b), sample(b + 1), sample(b + 2), sample(b + 3));
            }
        }
    }
    out.frames = {{img, 100}};
    out.width = w; out.height = h; out.loops = 0;
    return true;
}

bool writePam(QByteArray &out, const QImage &src, bool alpha, QString *) {
    const QImage img = alpha ? src.convertToFormat(QImage::Format_ARGB32) : flatten(src, Qt::white);
    const int w = img.width(), h = img.height(), depth = alpha ? 4 : 3;
    out = "P7\nWIDTH " + QByteArray::number(w) + "\nHEIGHT " + QByteArray::number(h) + "\nDEPTH " + QByteArray::number(depth) +
          "\nMAXVAL 255\nTUPLTYPE " + (alpha ? "RGB_ALPHA" : "RGB") + "\nENDHDR\n";
    out.reserve(out.size() + qsizetype(w) * h * depth);
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            out.append(char(qRed(s[x]))); out.append(char(qGreen(s[x]))); out.append(char(qBlue(s[x])));
            if (alpha) out.append(char(qAlpha(s[x])));
        }
    }
    return true;
}

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) { v = qBound(0.0f, v, 1.0f); return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

bool readPfm(const QByteArray &d, HdrImage &out, QString *err) {
    if (d.size() < 3 || d[0] != 'P' || (d[1] != 'F' && d[1] != 'f')) { setErr(err, QStringLiteral("PFM: bad magic")); return false; }
    const bool color = d[1] == 'F';
    int pos = 2;
    QList<QByteArray> toks;
    while (toks.size() < 3 && pos < d.size()) {
        while (pos < d.size() && isspace(uchar(d[pos]))) ++pos;
        const int st = pos;
        while (pos < d.size() && !isspace(uchar(d[pos]))) ++pos;
        if (pos > st) toks << d.mid(st, pos - st);
    }
    ++pos;   // single whitespace
    if (toks.size() < 3) { setErr(err, QStringLiteral("PFM: bad header")); return false; }
    const int w = toks[0].toInt(), h = toks[1].toInt();
    const double scale = toks[2].toDouble();
    const bool little = scale < 0;
    const int ch = color ? 3 : 1;
    if (w <= 0 || h <= 0 || qint64(w) * h > (1 << 27) || pos + qint64(w) * h * ch * 4 > d.size()) { setErr(err, QStringLiteral("PFM: bad size or truncated")); return false; }
    out.width = w; out.height = h;
    out.rgba.resize(qsizetype(w) * h * 4);
    const uchar *p = reinterpret_cast<const uchar *>(d.constData()) + pos;
    for (int y = 0; y < h; ++y) {
        const int dy = h - 1 - y;   // rows are stored bottom-to-top
        for (int x = 0; x < w; ++x) {
            float v[3];
            for (int c = 0; c < ch; ++c) {
                const uchar *b = p + ((qint64(y) * w + x) * ch + c) * 4;
                quint32 u = little ? (quint32(b[0]) | quint32(b[1]) << 8 | quint32(b[2]) << 16 | quint32(b[3]) << 24)
                                   : (quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | b[3]);
                std::memcpy(&v[c], &u, 4);
                if (!std::isfinite(v[c])) v[c] = 0;
            }
            float *o = out.rgba.data() + (qsizetype(dy) * w + x) * 4;
            o[0] = v[0]; o[1] = color ? v[1] : v[0]; o[2] = color ? v[2] : v[0]; o[3] = 1.0f;
        }
    }
    return true;
}

bool writePfm(QByteArray &out, const QImage &src, QString *) {
    const QImage img = flatten(src, Qt::white);
    const int w = img.width(), h = img.height();
    out = "PF\n" + QByteArray::number(w) + ' ' + QByteArray::number(h) + "\n-1.0\n";   // little-endian
    out.reserve(out.size() + qsizetype(w) * h * 12);
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgbToLinear(i / 255.0f);
    for (int y = h - 1; y >= 0; --y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const float v[3] = {lut[qRed(s[x])], lut[qGreen(s[x])], lut[qBlue(s[x])]};
            for (float f : v) { quint32 u; std::memcpy(&u, &f, 4); out.append(char(u)); out.append(char(u >> 8)); out.append(char(u >> 16)); out.append(char(u >> 24)); }
        }
    }
    return true;
}

} // namespace codec
