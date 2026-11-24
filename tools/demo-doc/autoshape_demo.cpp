// Writes a document with one of every Auto Shape (used for the README screenshots).
// Build: ninja autoshape_demo (not part of the default build). Usage: autoshape_demo out.lwd
#include "core/AutoShape.h"
#include "core/Document.h"
#include "io/LwdFormat.h"
#include <QGuiApplication>
#include <cstdio>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: autoshape_demo out.lwd\n"); return 2; }
    const QStringList kinds = autoshape::kinds();
    const int cols = 6, cw = 150, ch = 120;
    const int rows = (kinds.size() + cols - 1) / cols;
    Document *d = Document::createNew(cols * cw + 20, rows * ch + 20, QColor(250, 250, 250));
    const QColor fills[] = {QColor(66, 133, 244), QColor(219, 68, 55), QColor(244, 180, 0), QColor(15, 157, 88), QColor(171, 71, 188), QColor(0, 172, 193)};
    for (int i = 0; i < kinds.size(); ++i) {
        const QString k = kinds[i];
        const QRectF r(10 + (i % cols) * cw + 15, 10 + (i / cols) * ch + 15, cw - 30, ch - 30);
        VObject o;
        o.id = d->newId(); o.type = ObjType::AutoShape; o.rect = r;
        o.fillOn = !autoshape::isLineKind(k); o.fill = fills[i % 6];
        o.strokeOn = true; o.stroke = QColor(40, 40, 40);
        o.name = autoshape::label(k);
        autoshape::init(o, k);
        if (autoshape::isLineKind(k)) autoshape::setEndpoints(o, r.topLeft() + QPointF(0, r.height() * 0.7), r.bottomRight() - QPointF(0, r.height() * 0.7));
        d->addObject(o, -1, false);
    }
    QString err;
    if (!lwd::save(*d, QString::fromLocal8Bit(argv[1]), &err)) { fprintf(stderr, "%s\n", qPrintable(err)); return 1; }
    return 0;
}
