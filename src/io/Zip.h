#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>

// Minimal ZIP reader/writer (stored + deflate), enough for .lwd documents.
namespace zip {
struct Entry { QString name; QByteArray data; bool compress = true; };
QByteArray write(const QVector<Entry> &entries);
bool read(const QByteArray &zipData, QMap<QString, QByteArray> &out, QString *err = nullptr);
}
