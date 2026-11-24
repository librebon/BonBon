#pragma once
#include "AppState.h"
#include <QIcon>

// Tool / panel icons are drawn in code. The application icon may come from brand.qrc (Bonbon).
namespace icons {
QIcon tool(Tool t);
QIcon named(const QString &name); // eye, lock, plus, trash, up, down, play, stop, prev, next, dup, swap, default, fx, layer, frame, web, canvas
QPixmap appIcon(int size);
// Multi-size application icon (all embedded brand sizes, or the painted Lw/Bn pixmaps).
QIcon appIconIcon();
}

