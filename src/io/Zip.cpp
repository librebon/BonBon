#include "Zip.h"
#include <QVector>
#include <QtEndian>
#include <zlib.h>

namespace zip {
namespace {
void u16(QByteArray &b, quint16 v) { char t[2]; qToLittleEndian(v, t); b.append(t, 2); }
void u32(QByteArray &b, quint32 v) { char t[4]; qToLittleEndian(v, t); b.append(t, 4); }
quint16 g16(const QByteArray &b, qsizetype o) { return qFromLittleEndian<quint16>(b.constData() + o); }
quint32 g32(const QByteArray &b, qsizetype o) { return qFromLittleEndian<quint32>(b.constData() + o); }

QByteArray deflateRaw(const QByteArray &in) {
    z_stream zs{};
    deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    QByteArray out;
    out.resize(int(deflateBound(&zs, uLong(in.size()))));
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(int(zs.total_out));
    deflateEnd(&zs);
    return out;
}

bool inflateRaw(const QByteArray &in, qsizetype expected, QByteArray &out) {
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK) return false;
    out.resize(expected);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    int r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return r == Z_STREAM_END && qsizetype(zs.total_out) == expected;
}
}

QByteArray write(const QVector<Entry> &entries) {
    QByteArray out, central;
    for (const Entry &e : entries) {
        QByteArray name = e.name.toUtf8();
        quint32 crc = quint32(crc32(0, reinterpret_cast<const Bytef *>(e.data.constData()), uInt(e.data.size())));
        QByteArray body = e.compress ? deflateRaw(e.data) : e.data;
        quint16 method = e.compress ? 8 : 0;
        quint32 offset = quint32(out.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0x0800); u16(out, method);
        u16(out, 0); u16(out, 0x21); // time/date (1980-01-01)
        u32(out, crc); u32(out, quint32(body.size())); u32(out, quint32(e.data.size()));
        u16(out, quint16(name.size())); u16(out, 0);
        out += name; out += body;
        u32(central, 0x02014b50); u16(central, 0x031E); u16(central, 20); u16(central, 0x0800); u16(central, method);
        u16(central, 0); u16(central, 0x21);
        u32(central, crc); u32(central, quint32(body.size())); u32(central, quint32(e.data.size()));
        u16(central, quint16(name.size())); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
        u32(central, 0100644u << 16); u32(central, offset);
        central += name;
    }
    quint32 cdOffset = quint32(out.size());
    out += central;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, quint16(entries.size())); u16(out, quint16(entries.size()));
    u32(out, quint32(central.size())); u32(out, cdOffset); u16(out, 0);
    return out;
}

bool read(const QByteArray &d, QMap<QString, QByteArray> &out, QString *err) {
    auto fail = [&](const char *m) { if (err) *err = QString::fromLatin1(m); return false; };
    qsizetype eocd = -1;
    for (qsizetype i = d.size() - 22; i >= 0 && i >= d.size() - 22 - 65535; --i)
        if (g32(d, i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return fail("not a zip archive");
    int count = g16(d, eocd + 10);
    qsizetype p = g32(d, eocd + 16);
    for (int i = 0; i < count; ++i) {
        if (p + 46 > d.size() || g32(d, p) != 0x02014b50) return fail("corrupt central directory");
        quint16 method = g16(d, p + 10);
        quint32 crc = g32(d, p + 16), csize = g32(d, p + 20), usize = g32(d, p + 24);
        int nlen = g16(d, p + 28), xlen = g16(d, p + 30), clen = g16(d, p + 32);
        quint32 loff = g32(d, p + 42);
        QString name = QString::fromUtf8(d.mid(p + 46, nlen));
        p += 46 + nlen + xlen + clen;
        if (loff + 30 > quint32(d.size()) || g32(d, loff) != 0x04034b50) return fail("corrupt local header");
        qsizetype dataOff = loff + 30 + g16(d, loff + 26) + g16(d, loff + 28);
        if (dataOff + csize > d.size()) return fail("truncated entry");
        QByteArray body = d.mid(dataOff, csize), data;
        if (method == 0) data = body;
        else if (method == 8) { if (!inflateRaw(body, usize, data)) return fail("inflate failed"); }
        else return fail("unsupported compression method");
        if (quint32(crc32(0, reinterpret_cast<const Bytef *>(data.constData()), uInt(data.size()))) != crc) return fail("CRC mismatch");
        out.insert(name, data);
    }
    return true;
}
}
