#pragma once
#include "core/Document.h"
#include <QRect>
#include <QStringList>

// File > Export HTML and Images: slices the document into images and writes an HTML page
// using a table or CSS absolute positioning, with image maps for hotspots and JavaScript
// for simple rollovers and swap-image behaviours.
namespace html {
enum class Layout { Table, Css };

struct Options {
    Layout layout = Layout::Table;
    QString imagesDir = QStringLiteral("images"); // relative to the HTML file; empty = same folder
    QString title;                                 // page title (default: document title)
};

// One rectangular region of the sliced page: a user slice or an automatic filler slice.
struct Region {
    QRect rect;
    int row = 0, col = 0, rowSpan = 1, colSpan = 1; // grid position (table layout)
    int slice = -1;                                 // index into Plan::slices or -1 for auto
    QString name;                                   // image base name
};

struct Plan {
    QVector<const VObject *> slices;   // accepted (non-overlapping) slices
    QVector<int> xs, ys;               // grid lines
    QVector<Region> regions;           // covers the canvas exactly once
    QStringList warnings;
};

Plan plan(const Document &doc, const QString &baseName);
// v0.3.0: File > Export > Slices: every visible slice (overlapping ones included) as its own image, named
// after the slice, in its own optimize settings (or the document's). Animated where the format allows.
bool exportSlices(const Document &doc, const QString &dir, QString *err = nullptr, QStringList *writtenFiles = nullptr);
// v0.3.0: File > Export > Image Map: one image of the whole canvas plus an HTML page with a client-side
// <map>: an <area> per hotspot (rect / circle / poly) and per linked slice, with href / alt / target.
bool exportImageMap(const Document &doc, const QString &htmlPath, const Options &o, QString *err = nullptr,
                    QStringList *writtenFiles = nullptr);
// The <area> element for a hotspot or slice, coordinates relative to `origin`.
QString areaTag(const VObject &h, const QPointF &origin = {});
bool exportHtml(const Document &doc, const QString &htmlPath, const Options &o, QString *err = nullptr,
                QStringList *writtenFiles = nullptr);
}
