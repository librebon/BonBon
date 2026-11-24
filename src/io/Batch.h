#pragma once
// v0.3.0 Batch Process: an ordered list of steps run over many files, headless
// (File > Batch Process... and `libreworks --batch script.json files...`).
#include "ImageIO.h"
#include "core/Object.h"
#include <QColor>
#include <QJsonObject>
#include <QStringList>
#include <functional>

class Document;

namespace batch {

enum StepType {
    Export,         // write a file with an export preset (any, including C header) or explicit export options
    Scale,          // image size: exact size, percent, or fit within a box
    ReplaceColor,   // find & replace a colour (vector fills / strokes / gradient stops and bitmap pixels)
    ReplaceText,    // find & replace text in text objects
    Rename,         // output base name: prefix, suffix, numbering
    Command,        // a built-in command (flip, rotate, trim, fit canvas, flatten layers, ...)
    Filters,        // a filter chain (live effects in stack order; baked into bitmaps)
    Convert,        // write a file in another format with the document's own export settings
    kStepTypes
};
QStringList stepTypeNames();
QString stepTypeKey(int t);
int stepTypeFromKey(const QString &k);

// Built-in commands of the Command step.
QStringList commandKeys();      // "flip-h", "flip-v", "rotate-90", "rotate-180", "rotate-270", "trim", "fit-canvas", "flatten-layers", "grayscale", "invert", "auto-contrast"
QStringList commandLabels();

struct Step {
    int type = Export;
    bool enabled = true;
    // Export
    QString preset;                 // export preset name (io/ExportPresets.h); empty = `options`
    QJsonObject options;            // ExportOptions JSON (lwd::exportOptionsToJson) when no preset
    // Export / Convert: v0.3.1 Export Area -- write only this rectangle of each canvas (document px,
    // cut to the canvas; a file whose canvas misses it fails that step)
    bool useArea = false;
    QRect area;
    // Convert
    QString format;                 // format key (ImageIO::formatKey), e.g. "png", "jpeg", "cheader"
    // Scale
    int scaleMode = 1;              // 0 exact size, 1 percent, 2 fit within width x height
    int width = 0, height = 0;      // 0 = keep aspect from the other side (exact size)
    double percent = 50;
    bool onlyShrink = false;        // fit: never enlarge
    int resample = -1;              // v0.3.1: pixelscale Resample entry (-1 = Preferences, 4+ pixel-art scalers)
    // ReplaceColor
    QColor from = Qt::white, to = Qt::black;
    int tolerance = 0;              // 0..255, max per-channel difference (bitmaps)
    // ReplaceText
    QString find, replace;
    bool caseSensitive = true;
    // Rename
    QString prefix, suffix;
    QString baseName;               // empty = the file's own name
    bool numbering = false;
    int start = 1, digits = 3;
    // Command
    QString command;
    // Filters
    QVector<LiveEffect> effects;

    QString summary() const;
    QJsonObject toJson() const;
    static Step fromJson(const QJsonObject &j, bool *ok = nullptr);
};

struct Script {
    enum Input { Files, Folder, OpenDocuments };
    enum Output { ToFolder, Overwrite, Backup };
    int input = Files;
    QStringList files;              // Files
    QString folder;                 // Folder
    bool recursive = false;
    QString patterns = QStringLiteral("*.png *.jpg *.jpeg *.gif *.webp *.bmp *.tif *.tiff *.lwd *.bon *.psd *.ora *.svg");
    QVector<Step> steps;
    int output = ToFolder;
    QString outDir;                 // ToFolder (relative paths resolve against the script file)
    QString backupDir = QStringLiteral("Original Files");   // Backup: originals go here (next to the file)

    QJsonObject toJson() const;
    static Script fromJson(const QJsonObject &j, QString *err = nullptr);
    bool save(const QString &path, QString *err = nullptr) const;
    static Script load(const QString &path, QString *err = nullptr, bool *ok = nullptr);
};

// Resolves the script's input to a file list (Files / Folder; OpenDocuments is supplied by the GUI).
QStringList inputFiles(const Script &s);

// Apply one in-place step to a document (Scale / Replace* / Command / Filters). Returns false if it failed.
bool applyStep(Document &doc, const Step &st, QString *err = nullptr);

struct Result {
    int processed = 0, failed = 0;
    bool cancelled = false;
    QStringList written;            // output paths
    QStringList errors;             // "file: message"
    QStringList log;
};
// Progress: (index, total, current file). Return false to cancel.
using Progress = std::function<bool(int, int, const QString &)>;

// Runs the script over `files` (or inputFiles(s) when empty). `openDoc` lets the GUI supply already-open
// documents (OpenDocuments input): given a pseudo path it returns a document copy the batch may modify.
Result run(const Script &s, const QStringList &files = {}, const Progress &progress = {},
           const std::function<Document *(const QString &)> &openDoc = {});
// Processes one already-loaded document (takes no ownership). `sourcePath` names it for output/rename.
bool runOnDocument(const Script &s, Document &doc, const QString &sourcePath, int index, Result &res);

// The output path for `sourcePath` with base name `base` and extension `ext`.
QString outputPath(const Script &s, const QString &sourcePath, const QString &base, const QString &ext);
}
