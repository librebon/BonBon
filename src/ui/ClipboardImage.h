#pragma once
// What image (if any) is on the clipboard, and how big it is, without decoding it when that can be
// avoided. Used by File > New to size the canvas like Fireworks (v0.3.1).
#include <QByteArray>
#include <QSize>
#include <QString>
#include <functional>

class QMimeData;

namespace clipimage {

struct Info {
    QSize size;           // pixels; invalid = no image on the clipboard
    double dpi = 0;       // pixels/inch from the image's header; 0 = unknown
    QString source;       // "clipboard image", "copied objects", "image file <name>"
    QString format;       // the clipboard format or file the size came from
    QByteArray decodeLater; // set when only a full decode can give the size (decode it off the GUI thread)
    bool valid() const { return size.isValid() && !size.isEmpty(); }
};

// Libreworks' own Copy: the bounds (and the source document's resolution) of the objects or pixels
// copied in this process. MainWindow installs it; it is consulted only while the clipboard still
// holds that copy (the application/x-libreworks-objects marker).
struct Internal { QSize size; double dpi = 0; };
void setInternalProvider(std::function<Internal()> provider);

// Inspect `mime` (default: the system clipboard). Cheap except for the clipboard transfer itself:
// encoded images (PNG, JPEG, BMP, GIF, WebP, TIFF, ...) are sized from their header only, image
// files named by URL are sized from the file header, and our own copies come from the provider.
// `ownsClipboard`: the data lives in this process (QClipboard::ownsClipboard() or offscreen).
Info probe(const QMimeData *mime, bool ownsClipboard);
Info probeClipboard();

// Size and resolution from an encoded image's header (no pixel decode). dpi from PNG pHYs,
// JPEG JFIF / EXIF, BMP, TIFF; 0 when the header does not say.
Info fromEncoded(const QByteArray &data, const QString &formatHint = QString());
double headerDpi(const QByteArray &head);
// Full decode (worker-thread safe): for data whose header does not give a size.
Info decode(const QByteArray &data);
}
