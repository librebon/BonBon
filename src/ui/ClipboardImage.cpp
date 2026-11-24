#include "ClipboardImage.h"
#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QUrl>
#include <QtEndian>
#include <cmath>

namespace clipimage {

static std::function<Internal()> &provider() {
    static std::function<Internal()> p;
    return p;
}
void setInternalProvider(std::function<Internal()> p) { provider() = std::move(p); }

static const char *kObjMime = "application/x-libreworks-objects";

// --------------------------------------------------------------- header resolution
static double pngDpi(const QByteArray &d) {
    int pos = 8;
    while (pos + 12 <= d.size()) {
        const quint32 len = qFromBigEndian<quint32>(d.constData() + pos);
        const QByteArray type = d.mid(pos + 4, 4);
        if (type == "IDAT" || type == "IEND") break;
        if (type == "pHYs" && len >= 9 && pos + 8 + 9 <= d.size()) {
            const quint32 x = qFromBigEndian<quint32>(d.constData() + pos + 8);
            const uchar unit = uchar(d[pos + 16]);
            if (unit == 1 && x > 0) return x * 0.0254;
            return 0;
        }
        if (len > quint32(d.size())) break;
        pos += 12 + int(len);
    }
    return 0;
}

static double tiffDpi(const QByteArray &d, int base, bool *found = nullptr) {
    // TIFF / EXIF IFD0: XResolution (282) RATIONAL, ResolutionUnit (296): 2 inch, 3 cm
    if (found) *found = false;
    if (base + 8 > d.size()) return 0;
    const bool le = d.mid(base, 2) == "II";
    if (!le && d.mid(base, 2) != "MM") return 0;
    auto u16 = [&](int p) -> quint32 { if (p + 2 > d.size()) return 0; return le ? qFromLittleEndian<quint16>(d.constData() + p) : qFromBigEndian<quint16>(d.constData() + p); };
    auto u32 = [&](int p) -> quint32 { if (p + 4 > d.size()) return 0; return le ? qFromLittleEndian<quint32>(d.constData() + p) : qFromBigEndian<quint32>(d.constData() + p); };
    const quint32 ifd = u32(base + 4);
    if (ifd == 0 || base + int(ifd) + 2 > d.size()) return 0;
    const int n = int(u16(base + int(ifd)));
    double xres = 0;
    int unit = 2;
    for (int i = 0; i < n; ++i) {
        const int e = base + int(ifd) + 2 + i * 12;
        if (e + 12 > d.size()) break;
        const quint32 tag = u16(e);
        if (tag == 282) {
            const quint32 off = u32(e + 8);
            const quint32 num = u32(base + int(off)), den = u32(base + int(off) + 4);
            if (den) xres = double(num) / den;
        } else if (tag == 296) unit = int(u16(e + 8));
    }
    if (xres <= 0 || unit == 1) return 0;
    if (found) *found = true;
    return unit == 3 ? xres * 2.54 : xres;
}

static double jpegDpi(const QByteArray &d) {
    int pos = 2;
    double exif = 0;
    while (pos + 4 <= d.size()) {
        if (uchar(d[pos]) != 0xFF) break;
        const uchar m = uchar(d[pos + 1]);
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { pos += 2; continue; }
        if (m == 0xDA || m == 0xD9) break;   // scan data: no more header
        const int len = int(qFromBigEndian<quint16>(d.constData() + pos + 2));
        const int seg = pos + 4;
        if (m == 0xE0 && len >= 14 && d.mid(seg, 5) == QByteArray("JFIF\0", 5) && seg + 12 <= d.size()) {
            const uchar unit = uchar(d[seg + 7]);
            const int x = int(qFromBigEndian<quint16>(d.constData() + seg + 8));
            if (unit == 1 && x > 0) return x;
            if (unit == 2 && x > 0) return x * 2.54;
        } else if (m == 0xE1 && len > 14 && d.mid(seg, 6) == QByteArray("Exif\0\0", 6)) {
            exif = tiffDpi(d, seg + 6);
        }
        pos += 2 + len;
    }
    return exif;
}

double headerDpi(const QByteArray &d) {
    if (d.startsWith("\x89PNG\r\n\x1a\n")) return pngDpi(d);
    if (d.startsWith("\xFF\xD8")) return jpegDpi(d);
    if (d.startsWith("BM") && d.size() >= 46) {
        const qint32 ppm = qFromLittleEndian<qint32>(d.constData() + 38);
        return ppm > 0 ? ppm * 0.0254 : 0;
    }
    if (d.startsWith("II*") || d.startsWith("MM\0*")) return tiffDpi(d, 0);
    return 0;
}

static double sane(double dpi) {
    if (!(dpi >= 1 && dpi <= 100000)) return 0;
    if (std::abs(dpi - std::round(dpi)) < 0.05) return std::round(dpi);   // pixels-per-metre rounding (300 ppi = 11811 ppm)
    return std::round(dpi * 100) / 100;
}

Info fromEncoded(const QByteArray &data, const QString &formatHint) {
    Info r;
    if (data.isEmpty()) return r;
    r.format = formatHint;
    // PNG (what X11 and Wayland apps offer first): IHDR is the first chunk, read it directly
    if (data.size() >= 24 && data.startsWith("\x89PNG\r\n\x1a\n") && data.mid(12, 4) == "IHDR") {
        const quint32 w = qFromBigEndian<quint32>(data.constData() + 16), h = qFromBigEndian<quint32>(data.constData() + 20);
        if (w > 0 && h > 0 && w <= 0x7FFFFFFF && h <= 0x7FFFFFFF) {
            r.size = QSize(int(w), int(h));
            r.dpi = sane(headerDpi(data.left(256 * 1024)));
            return r;
        }
    }
    QBuffer buf(const_cast<QByteArray *>(&data));
    buf.open(QIODevice::ReadOnly);
    QImageReader rd(&buf);
    rd.setAutoTransform(true);   // EXIF orientation swaps the size, as a paste would
    const QSize s = rd.size();   // header only
    r.format = formatHint;
    if (s.isValid() && !s.isEmpty()) {
        r.size = s;
        if (rd.transformation() & QImageIOHandler::TransformationRotate90) r.size.transpose();
        r.dpi = sane(headerDpi(data.left(256 * 1024)));
    } else if (rd.canRead()) {
        r.decodeLater = data;   // some formats only know their size after decoding
    }
    return r;
}

Info decode(const QByteArray &data) {
    Info r;
    QImage img = QImage::fromData(data);
    if (img.isNull()) return r;
    r.size = img.size();
    r.dpi = sane(headerDpi(data.left(256 * 1024)));
    return r;
}

static bool isEncodedImageFormat(const QString &f) {
    return f.startsWith(QLatin1String("image/")) && f != QLatin1String("image/svg+xml") && !f.startsWith(QLatin1String("image/x-qt"));
}

static Info fromFile(const QString &path) {
    Info r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return r;
    const QByteArray head = f.read(256 * 1024);
    f.seek(0);
    QImageReader rd(&f);
    rd.setAutoTransform(true);
    const QSize s = rd.size();
    if (!s.isValid() || s.isEmpty()) return r;
    r.size = s;
    if (rd.transformation() & QImageIOHandler::TransformationRotate90) r.size.transpose();
    r.dpi = sane(headerDpi(head));
    r.source = QStringLiteral("image file %1").arg(QFileInfo(path).fileName());
    r.format = QStringLiteral("text/uri-list");
    return r;
}

Info probe(const QMimeData *mime, bool ownsClipboard) {
    Info none;
    if (!mime) return none;
    const QStringList formats = mime->formats();
    // 1. Libreworks' own copy (objects or a pixel selection) in this process
    if (mime->hasFormat(QString::fromLatin1(kObjMime)) && provider()) {
        const Internal in = provider()();
        if (in.size.isValid() && !in.size.isEmpty()) {
            Info r;
            r.size = in.size;
            r.dpi = sane(in.dpi);
            r.source = QStringLiteral("copied objects");
            r.format = QString::fromLatin1(kObjMime);
            return r;
        }
    }
    // 2. Data living in this process: the QImage is already decoded, reading it is free
    if (ownsClipboard && mime->hasImage()) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            Info r;
            r.size = img.size();
            // a QImage made in memory carries the screen's default resolution, not a real one
            static const int defDpm = QImage(1, 1, QImage::Format_ARGB32).dotsPerMeterX();
            if (img.dotsPerMeterX() > 0 && img.dotsPerMeterX() != defDpm) r.dpi = sane(img.dotsPerMeterX() * 0.0254);
            r.source = QStringLiteral("clipboard image");
            r.format = QStringLiteral("application/x-qt-image");
            return r;
        }
    }
    // 3. Encoded image from another application: transfer it once, read only its header.
    //    Prefer formats whose header is at the start and cheap to parse.
    static const QStringList preferred = {QStringLiteral("image/png"), QStringLiteral("image/jpeg"), QStringLiteral("image/bmp"),
                                          QStringLiteral("image/x-bmp"), QStringLiteral("image/gif"), QStringLiteral("image/webp"),
                                          QStringLiteral("image/tiff"), QStringLiteral("image/avif")};
    QStringList order;
    for (const QString &f : preferred) if (formats.contains(f)) order << f;
    for (const QString &f : formats) if (isEncodedImageFormat(f) && !order.contains(f)) order << f;
    for (const QString &f : order) {
        Info r = fromEncoded(mime->data(f), f);
        if (r.valid() || !r.decodeLater.isEmpty()) { r.source = QStringLiteral("clipboard image"); return r; }
    }
    // 4. Copied image files (file managers put file:// URLs on the clipboard)
    if (mime->hasUrls()) {
        for (const QUrl &u : mime->urls()) {
            if (!u.isLocalFile()) continue;
            Info r = fromFile(u.toLocalFile());
            if (r.valid()) return r;
        }
    }
    // 5. Anything else Qt can turn into an image (in-process only reaches here when not owned)
    if (!ownsClipboard && order.isEmpty() && mime->hasImage()) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            Info r;
            r.size = img.size();
            r.source = QStringLiteral("clipboard image");
            r.format = QStringLiteral("application/x-qt-image");
            return r;
        }
    }
    return none;
}

Info probeClipboard() {
    QClipboard *cb = QGuiApplication::clipboard();
    if (!cb) return {};
    const bool own = cb->ownsClipboard() || QGuiApplication::platformName() == QLatin1String("offscreen")
                     || QGuiApplication::platformName() == QLatin1String("minimal");
    return probe(cb->mimeData(QClipboard::Clipboard), own);
}
}
