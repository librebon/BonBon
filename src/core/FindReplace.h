#pragma once
// Find & Replace (v0.3.1): the engine behind Window > Find & Replace and the Batch "Replace colour" /
// "Replace text" steps. Finds and replaces text, fonts, colours (fills, strokes, gradient stops, effect
// colours and optionally bitmap pixels), URLs and non-web-safe colours in a document's objects.
#include "Object.h"
#include <QColor>
#include <QSet>
#include <QString>
#include <QVector>

class Document;
struct DocState;

namespace fr {

enum class What { Text = 0, Font = 1, Color = 2, Url = 3, NonWebColor = 4 };
enum class Scope { Document = 0, Selection = 1, AllFrames = 2, AllDocuments = 3 };
QStringList whatNames();
QStringList scopeNames();

struct Query {
    What what = What::Text;
    // text / URL
    QString find, replace;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
    // font: match the family (empty = any) and the size (0 = any); replace sets the non-empty / non-zero parts
    QString fontFrom, fontTo;
    double sizeFrom = 0, sizeTo = 0;
    // colour
    QColor from = Qt::white, to = Qt::black;
    int tolerance = 0;              // 0..255, max per-channel difference
    bool fills = true, strokes = true, gradients = true, effects = true;
    bool pixels = false;            // bitmap pixels too
    bool canvas = false;            // the canvas colour (Batch)
    // Batch-compatible pixel replacement: no xform baking, alpha scaled by To's alpha (always on)
};

// One match: an object of a frame (layer / index for ordering), or the canvas colour (objectId 0).
struct Hit {
    int frame = 0, layer = 0, index = 0;
    quint64 objectId = 0;
    QString detail;    // what matched, for the panel's list
};

bool isWebSafe(QRgb c);
QRgb nearestWebSafe(QRgb c);
// What in `o` matches (empty = nothing); `o` is left alone.
QString matchIn(const VObject &o, const Query &q);
// Replaces in `o`; returns the number of replacements (0 = unchanged).
int replaceIn(VObject &o, const Query &q);

// Matches in scope, in frame / layer / stacking order. Selection = the selected objects of the current
// frame; Document = the current frame; AllFrames / AllDocuments = every frame.
// Objects on locked layers are skipped. `selection` (Selection scope) overrides the document's selection,
// so a search can step through what was selected when it started.
QVector<Hit> findAll(const Document &doc, const Query &q, Scope scope, const QSet<quint64> *selection = nullptr);
// Replaces every match in scope (no commit); returns the replacement count.
int replaceAll(Document &doc, const Query &q, Scope scope, const QSet<quint64> *selection = nullptr);
// Replaces in one object of a frame; returns the count.
int replaceHit(Document &doc, const Query &q, const Hit &h);
// The same over a whole state (all frames, every object, optionally the canvas colour): Batch steps.
int replaceAllInState(DocState &s, const Query &q);
}
