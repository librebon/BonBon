#pragma once
#include "ImageIO.h"
#include "Codecs.h"
#include <QColor>
#include <QStringList>
#include <functional>

class Document;
struct DocState;

// Document-level import / export: layered files (PSD / ORA / XCF), SVG / SVGZ (vector objects),
// PDF export, the editable PNG (Libreworks "lwDC" chunk) and the favicon / app-icon bundle.
namespace docio {
// Formats that open as a layered / vector document instead of a flat bitmap.
bool opensAsDocument(Fmt f);
// Open `path` as a document. Returns nullptr and sets *err on failure; *notes lists anything that
// was approximated or skipped (shown to the user).
Document *open(const QString &path, QString *err, QStringList *notes = nullptr);
Document *openData(const QByteArray &data, Fmt fmt, const QString &baseDir, QString *err, QStringList *notes = nullptr);
// Export the document (frame `frame`; PDF: every frame, one page each) as `o.format`. Layered and
// vector formats keep layers / objects; everything else is rendered and passed to ImageIO.
bool exportDocument(const Document &doc, int frame, const ExportOptions &o, QByteArray &out, QString *err, QStringList *notes = nullptr);
bool exportDocumentFile(const Document &doc, int frame, const ExportOptions &o, const QString &path, QString *err, QStringList *notes = nullptr);
// Each visible, non-web layer rendered at canvas size (trimmed); a layer holding one object takes that
// object's opacity / blend mode.
codec::LayeredImage layersFromState(const DocState &st, int frame);

// ---- SVG
bool svgToState(const QByteArray &svg, DocState &st, const std::function<quint64()> &newId, const QString &baseDir,
                QStringList *notes, QString *err);
QByteArray stateToSvg(const DocState &st, int frame, bool *pureVector = nullptr, QStringList *notes = nullptr);
bool isVectorDocument(const DocState &st, int frame);   // every visible object exports as SVG vectors
bool isGzip(const QByteArray &d);
QByteArray gzip(const QByteArray &d);
QByteArray gunzip(const QByteArray &d, bool *ok = nullptr);

// ---- PDF
bool writePdf(const DocState &st, QByteArray &out, QString *err);

// ---- editable PNG
bool isEditablePng(const QByteArray &d);

// ---- favicon / app icons
struct FaviconOptions {
    int icoSizes = 0x7;           // bit 0: 16, bit 1: 32, bit 2: 48
    bool svg = true;              // write favicon.svg when the document is vector
    int applePadding = 0;         // apple-touch-icon padding, % of the icon (0..40)
    QColor appleBackground = Qt::white; // apple-touch-icon background (iOS shows transparency as black)
    QString appName;              // site.webmanifest "name" (default: document title)
    QColor themeColor = Qt::white;
    QString basePath = QStringLiteral("/");   // URL prefix used in the snippet and manifest
};
struct FaviconResult {
    QStringList files;            // written file names
    QString snippet;              // the <link> tags (also written as favicon-snippet.html)
    bool wroteSvg = false;
};
QImage faviconImage(const DocState &st, int frame, int size, int paddingPct = 0, const QColor &bg = Qt::transparent);
bool exportFaviconBundle(const DocState &st, int frame, const QString &dir, const FaviconOptions &o, FaviconResult *res, QString *err);
}
