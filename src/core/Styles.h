#pragma once
#include "Object.h"
#include <QImage>
#include <QJsonObject>
#include <QVector>

// Fireworks Styles: a named set of fill, stroke, Live Effects and text attributes that can be
// applied to objects. Built-in presets ship with the app; user styles live in
// $XDG_CONFIG_HOME/libreworks/styles.json (default ~/.config/libreworks/styles.json).
struct Style {
    QString name;
    bool builtin = false;
    // which attributes the style carries (Fireworks' New Style dialog checkboxes)
    bool hasFill = true, hasStroke = true, hasEffects = true, hasText = false;
    VObject attrs;  // a Rect (or Text) carrying the attributes

    static Style fromObject(const VObject &o, const QString &name);
    // Applies the style; returns false when nothing changed. Fill/stroke are skipped on bitmaps.
    bool applyTo(VObject &o) const;
};

namespace styles {
QVector<Style> builtins();
QString userFile();
QVector<Style> loadUser(QString *err = nullptr);                      // the user-made styles only
bool saveUser(const QVector<Style> &user, QString *err = nullptr);   // writes the non-builtins only
// The Styles panel's whole library, in panel order: built-ins may be renamed, reordered or deleted.
// A version-2 file keeps the complete list; a version-1 file (or none) gives built-ins + user styles.
QVector<Style> loadLibrary(QString *err = nullptr);
bool saveLibrary(const QVector<Style> &all, QString *err = nullptr);
// .lwstyles exchange files (same JSON as the library); imported styles become user styles
bool exportFile(const QString &path, const QVector<Style> &list, QString *err = nullptr);
QVector<Style> importFile(const QString &path, QString *err = nullptr);
// one-line description: "Fill, Stroke, Effects: Drop Shadow, Text"
QString describe(const Style &s);
QJsonObject toJson(const Style &s);
bool fromJson(const QJsonObject &j, Style &s);
// swatch: the style on a rounded square, or on "Aa" for text styles
QImage preview(const Style &s, int size = 44);
}
