#include "Brand.h"
// Radiance RGBE (.hdr / .pic): reads new-style adaptive RLE scanlines, old-style RLE and flat pixels,
// "-Y h +X w" and the other orientations; writes new-style RLE. Plus OpenEXR (RGBA, any compression,
// half or float) through the OpenEXR library, and the tone mapping used to bring HDR data to 8 bits.
#include "Codecs.h"
#include <cmath>
#include <cstring>
#ifdef LW_HAVE_OPENEXR
#include <ImfArray.h>
#include <ImfFrameBuffer.h>
#include <ImfOutputFile.h>
#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfRgbaFile.h>
#include <ImfStandardAttributes.h>
#include <ImfVersion.h>
#include <OpenEXRConfig.h>
#endif

namespace codec {

namespace {
void rgbeToFloat(const uchar *c, float *o) {
    if (c[3] == 0) { o[0] = o[1] = o[2] = 0; return; }
    const float f = std::ldexp(1.0f, int(c[3]) - (128 + 8));
    o[0] = (c[0] + 0.5f) * f; o[1] = (c[1] + 0.5f) * f; o[2] = (c[2] + 0.5f) * f;
}
void floatToRgbe(const float *v, uchar *c) {
    const float m = std::max({v[0], v[1], v[2]});
    if (m < 1e-32f) { c[0] = c[1] = c[2] = c[3] = 0; return; }
    int e;
    const float f = std::frexp(m, &e) * 256.0f / m;
    c[0] = uchar(qBound(0.0f, v[0] * f, 255.0f)); c[1] = uchar(qBound(0.0f, v[1] * f, 255.0f)); c[2] = uchar(qBound(0.0f, v[2] * f, 255.0f));
    c[3] = uchar(e + 128);
}
}

bool readRgbe(const QByteArray &d, HdrImage &out, QString *err) {
    if (!d.startsWith("#?")) { setErr(err, QStringLiteral("HDR: bad signature")); return false; }
    int pos = 0;
    bool rgbe = true;
    float exposure = 1.0f;
    // header lines until an empty line
    while (true) {
        const int eol = d.indexOf('\n', pos);
        if (eol < 0) { setErr(err, QStringLiteral("HDR: truncated header")); return false; }
        const QByteArray line = d.mid(pos, eol - pos).trimmed();
        pos = eol + 1;
        if (line.isEmpty()) break;
        if (line.startsWith("FORMAT=")) rgbe = line.mid(7) == "32-bit_rle_rgbe";
        if (line.startsWith("EXPOSURE=")) { const float e = line.mid(9).toFloat(); if (e > 0) exposure *= e; }
    }
    if (!rgbe) { setErr(err, QStringLiteral("HDR: only RGBE is supported (not XYZE)")); return false; }
    const int eol = d.indexOf('\n', pos);
    if (eol < 0) { setErr(err, QStringLiteral("HDR: missing resolution")); return false; }
    const QList<QByteArray> res = d.mid(pos, eol - pos).trimmed().split(' ');
    pos = eol + 1;
    if (res.size() != 4) { setErr(err, QStringLiteral("HDR: bad resolution line")); return false; }
    // standard: "-Y h +X w"; others are flips / transposes
    const bool yFirst = res[0].endsWith('Y');
    const int n1 = res[1].toInt(), n2 = res[3].toInt();
    const bool s1neg = res[0].startsWith('-'), s2pos = res[2].startsWith('+');
    const int h = yFirst ? n1 : n2, w = yFirst ? n2 : n1;
    if (w <= 0 || h <= 0 || qint64(w) * h > (1 << 27)) { setErr(err, QStringLiteral("HDR: bad size")); return false; }
    const int lineLen = yFirst ? w : h, lines = yFirst ? h : w;
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 n = d.size();
    out.width = w; out.height = h;
    out.rgba.assign(qsizetype(w) * h * 4, 0.0f);
    std::vector<uchar> scan(size_t(lineLen) * 4);
    for (int li = 0; li < lines; ++li) {
        if (pos + 4 > n) { setErr(err, QStringLiteral("HDR: truncated data")); return false; }
        if (lineLen >= 8 && lineLen < 32768 && p[pos] == 2 && p[pos + 1] == 2 && !(p[pos + 2] & 0x80)) {
            if (((p[pos + 2] << 8) | p[pos + 3]) != lineLen) { setErr(err, QStringLiteral("HDR: bad scanline length")); return false; }
            pos += 4;
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < lineLen) {
                    if (pos >= n) { setErr(err, QStringLiteral("HDR: truncated data")); return false; }
                    int cnt = p[pos++];
                    if (cnt > 128) {
                        cnt -= 128;
                        if (pos >= n || x + cnt > lineLen) { setErr(err, QStringLiteral("HDR: bad run")); return false; }
                        const uchar v = p[pos++];
                        for (int i = 0; i < cnt; ++i) scan[size_t(x++) * 4 + c] = v;
                    } else {
                        if (cnt == 0 || pos + cnt > n || x + cnt > lineLen) { setErr(err, QStringLiteral("HDR: bad literal")); return false; }
                        for (int i = 0; i < cnt; ++i) scan[size_t(x++) * 4 + c] = p[pos++];
                    }
                }
            }
        } else {
            // flat or old-style RLE (1,1,1,count repeats the previous pixel)
            int x = 0, shift = 0;
            while (x < lineLen) {
                if (pos + 4 > n) { setErr(err, QStringLiteral("HDR: truncated data")); return false; }
                if (p[pos] == 1 && p[pos + 1] == 1 && p[pos + 2] == 1 && x > 0) {
                    const int cnt = p[pos + 3] << shift;
                    for (int i = 0; i < cnt && x < lineLen; ++i, ++x) std::memcpy(&scan[size_t(x) * 4], &scan[size_t(x - 1) * 4], 4);
                    shift += 8;
                } else { std::memcpy(&scan[size_t(x) * 4], p + pos, 4); ++x; shift = 0; }
                pos += 4;
            }
        }
        for (int i = 0; i < lineLen; ++i) {
            int x, y;
            if (yFirst) { y = s1neg ? li : h - 1 - li; x = s2pos ? i : w - 1 - i; }
            else { x = s1neg ? li : w - 1 - li; y = s2pos ? i : h - 1 - i; }
            float *o = out.rgba.data() + (qsizetype(y) * w + x) * 4;
            rgbeToFloat(&scan[size_t(i) * 4], o);
            o[0] /= exposure; o[1] /= exposure; o[2] /= exposure; o[3] = 1.0f;
        }
    }
    return true;
}

bool writeRgbe(QByteArray &out, const QImage &src, QString *) {
    const QImage img = flatten(src, Qt::white);
    const int w = img.width(), h = img.height();
    out = "#?RADIANCE\n# Created by " LW_PRODUCT_NAME "\nFORMAT=32-bit_rle_rgbe\n\n-Y " + QByteArray::number(h) + " +X " + QByteArray::number(w) + "\n";
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgbToLinear(i / 255.0f);
    std::vector<uchar> scan(size_t(w) * 4);
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) { const float v[3] = {lut[qRed(s[x])], lut[qGreen(s[x])], lut[qBlue(s[x])]}; floatToRgbe(v, &scan[size_t(x) * 4]); }
        if (w < 8 || w >= 32768) { out.append(reinterpret_cast<const char *>(scan.data()), qsizetype(scan.size())); continue; }
        out.append(char(2)); out.append(char(2)); out.append(char(w >> 8)); out.append(char(w & 255));
        for (int c = 0; c < 4; ++c) {
            int x = 0;
            while (x < w) {
                // find next run of >= 4
                int beg = x, run = 0;
                while (beg < w) {
                    run = 1;
                    while (beg + run < w && run < 127 && scan[size_t(beg + run) * 4 + c] == scan[size_t(beg) * 4 + c]) ++run;
                    if (run >= 4) break;
                    beg += run;
                }
                if (beg >= w) { beg = w; run = 0; }
                while (x < beg) {   // literals before the run
                    const int cnt = qMin(128, beg - x);
                    out.append(char(cnt));
                    for (int i = 0; i < cnt; ++i) out.append(char(scan[size_t(x + i) * 4 + c]));
                    x += cnt;
                }
                if (run >= 4) { out.append(char(128 + run)); out.append(char(scan[size_t(beg) * 4 + c])); x = beg + run; }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- OpenEXR
#ifdef LW_HAVE_OPENEXR
namespace {
class MemIStream : public Imf::IStream {
public:
    MemIStream(const QByteArray &d) : Imf::IStream("memory"), m_d(d) {}
    bool isMemoryMapped() const override { return false; }
    bool read(char c[], int n) override {
        if (m_pos + n > quint64(m_d.size())) throw std::runtime_error("EXR: unexpected end of data");
        std::memcpy(c, m_d.constData() + m_pos, size_t(n)); m_pos += quint64(n);
        return m_pos < quint64(m_d.size());
    }
    uint64_t tellg() override { return m_pos; }
    void seekg(uint64_t pos) override { m_pos = pos; }
    void clear() override {}
private:
    const QByteArray &m_d;
    quint64 m_pos = 0;
};
class MemOStream : public Imf::OStream {
public:
    MemOStream(QByteArray &d) : Imf::OStream("memory"), m_d(d) {}
    void write(const char c[], int n) override {
        if (m_pos + quint64(n) > quint64(m_d.size())) m_d.resize(qsizetype(m_pos + quint64(n)));
        std::memcpy(m_d.data() + m_pos, c, size_t(n)); m_pos += quint64(n);
    }
    uint64_t tellp() override { return m_pos; }
    void seekp(uint64_t pos) override { m_pos = pos; }
private:
    QByteArray &m_d;
    quint64 m_pos = 0;
};
}
#endif

bool readExr(const QByteArray &d, HdrImage &out, QString *err) {
#ifdef LW_HAVE_OPENEXR
    try {
        MemIStream is(d);
        Imf::RgbaInputFile f(is);
        const Imath::Box2i dw = f.dataWindow();
        const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0 || qint64(w) * h > (1 << 27)) { setErr(err, QStringLiteral("EXR: bad size")); return false; }
        Imf::Array2D<Imf::Rgba> px(h, w);
        f.setFrameBuffer(&px[0][0] - dw.min.x - qint64(dw.min.y) * w, 1, size_t(w));
        f.readPixels(dw.min.y, dw.max.y);
        const bool hasAlpha = f.channels() & Imf::WRITE_A;
        const bool luminance = !(f.channels() & (Imf::WRITE_R | Imf::WRITE_G | Imf::WRITE_B)) && (f.channels() & Imf::WRITE_Y);
        Q_UNUSED(luminance);   // RgbaInputFile converts Y / YC to RGB already
        out.width = w; out.height = h;
        out.rgba.resize(qsizetype(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const Imf::Rgba &c = px[y][x];
                float *o = out.rgba.data() + (qsizetype(y) * w + x) * 4;
                const float a = hasAlpha ? float(c.a) : 1.0f;
                // EXR stores premultiplied colour: un-premultiply for our straight-alpha pipeline
                const float k = a > 1e-6f ? 1.0f / a : 0.0f;
                o[0] = std::isfinite(float(c.r)) ? float(c.r) * (hasAlpha ? k : 1) : 0;
                o[1] = std::isfinite(float(c.g)) ? float(c.g) * (hasAlpha ? k : 1) : 0;
                o[2] = std::isfinite(float(c.b)) ? float(c.b) * (hasAlpha ? k : 1) : 0;
                o[3] = qBound(0.0f, a, 1.0f);
            }
        return true;
    } catch (const std::exception &e) {
        setErr(err, QStringLiteral("EXR: %1").arg(QString::fromLocal8Bit(e.what())));
        return false;
    }
#else
    Q_UNUSED(d); Q_UNUSED(out);
    setErr(err, QStringLiteral("EXR support not built (OpenEXR missing)"));
    return false;
#endif
}

bool writeExr(QByteArray &out, const QImage &src, const ExportOptions &o, QString *err) {
#ifdef LW_HAVE_OPENEXR
    try {
        const QImage img = src.convertToFormat(QImage::Format_ARGB32);
        const int w = img.width(), h = img.height();
        static const Imf::Compression comps[] = {Imf::NO_COMPRESSION, Imf::ZIP_COMPRESSION, Imf::PIZ_COMPRESSION, Imf::RLE_COMPRESSION,
                                                 Imf::ZIPS_COMPRESSION, Imf::PXR24_COMPRESSION, Imf::B44_COMPRESSION, Imf::DWAA_COMPRESSION};
        Imf::Header hdr(w, h, 1.0f, Imath::V2f(0, 0), 1.0f, Imf::INCREASING_Y, comps[qBound(0, o.exrCompression, 7)]);
        out.clear();
        float lut[256];
        for (int i = 0; i < 256; ++i) lut[i] = srgbToLinear(i / 255.0f);
        const bool alpha = o.alpha;
        if (o.exrHalf) {
            MemOStream os(out);
            Imf::RgbaOutputFile f(os, hdr, alpha ? Imf::WRITE_RGBA : Imf::WRITE_RGB);
            Imf::Array2D<Imf::Rgba> px(h, w);
            const QImage s = alpha ? img : flatten(img, o.matte);
            for (int y = 0; y < h; ++y) {
                const QRgb *r = reinterpret_cast<const QRgb *>(s.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    const float a = qAlpha(r[x]) / 255.0f;
                    px[y][x] = Imf::Rgba(lut[qRed(r[x])] * a, lut[qGreen(r[x])] * a, lut[qBlue(r[x])] * a, a);   // premultiplied
                }
            }
            f.setFrameBuffer(&px[0][0], 1, size_t(w));
            f.writePixels(h);
        } else {
            // 32-bit float channels through the general FrameBuffer API
            const char *names[] = {"R", "G", "B", "A"};
            const int nch = alpha ? 4 : 3;
            for (int c = 0; c < nch; ++c) hdr.channels().insert(names[c], Imf::Channel(Imf::FLOAT));
            std::vector<float> planes(size_t(w) * h * nch);
            const QImage s = alpha ? img : flatten(img, o.matte);
            for (int y = 0; y < h; ++y) {
                const QRgb *r = reinterpret_cast<const QRgb *>(s.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    const float a = qAlpha(r[x]) / 255.0f;
                    const size_t i = size_t(y) * w + x;
                    planes[i] = lut[qRed(r[x])] * a;
                    planes[size_t(w) * h + i] = lut[qGreen(r[x])] * a;
                    planes[2 * size_t(w) * h + i] = lut[qBlue(r[x])] * a;
                    if (alpha) planes[3 * size_t(w) * h + i] = a;
                }
            }
            Imf::FrameBuffer fb;
            for (int c = 0; c < nch; ++c)
                fb.insert(names[c], Imf::Slice(Imf::FLOAT, reinterpret_cast<char *>(planes.data() + size_t(c) * w * h), sizeof(float), sizeof(float) * size_t(w)));
            MemOStream os(out);
            Imf::OutputFile f(os, hdr);
            f.setFrameBuffer(fb);
            f.writePixels(h);
        }
        return true;
    } catch (const std::exception &e) {
        setErr(err, QStringLiteral("EXR: %1").arg(QString::fromLocal8Bit(e.what())));
        return false;
    }
#else
    Q_UNUSED(out); Q_UNUSED(src); Q_UNUSED(o);
    setErr(err, QStringLiteral("EXR support not built (OpenEXR missing)"));
    return false;
#endif
}

} // namespace codec
