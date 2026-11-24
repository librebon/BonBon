#pragma once
#include <QJsonObject>
#include <QString>
#include "core/Object.h"
#include "io/ImageIO.h"
class Document;
struct DocState;

// Libreworks native document (.lwd): a ZIP holding document.json + images/<id>.png.
// See docs/LWD_FORMAT.md.
namespace lwd {
bool save(const Document &doc, const QString &path, QString *err = nullptr);
QByteArray serialize(const Document &doc);
Document *load(const QString &path, QString *err = nullptr);
Document *deserialize(const QByteArray &data, QString *err = nullptr);
// JSON pieces of the format, shared with the Styles and Library files
QJsonObject effectToJson(const LiveEffect &e);
LiveEffect effectFromJson(const QJsonObject &j, bool *ok = nullptr);
QJsonObject objectToJson(const VObject &o);      // vector objects only
bool objectFromJson(const QJsonObject &j, VObject &o);
// A digest of everything render::renderFrame(st, frame, ...) draws: the frame's objects (bitmaps by
// QImage cache key), the layer flags, the Library and the canvas. Equal keys = equal renders.
QByteArray contentKey(const DocState &st, int frame);
// Library (symbols: id, name, kind, frames) hashed once for historyDigest.
QByteArray libraryKey(const DocState &st);
// One frame's layer flags and objects, without the Library (so symbols are not re-hashed per frame).
QByteArray frameLayerKey(const DocState &st, int frame);
// Digest of one object's drawable content (bitmaps by QImage::cacheKey). Equal keys =
// equal render::renderObject pixels for the same clip. Opacity / blend / name / id / locked
// / visible are omitted: drawObject applies opacity and blend at blit time.
QByteArray objectContentKey(const VObject &o);
// export / optimize settings (document "export", "previewPanes", slice "optimize")
QJsonObject exportOptionsToJson(const ExportOptions &e);
ExportOptions exportOptionsFromJson(const QJsonObject &j);
}
