#include "ui/Theme.h"
#include "ui/Panels.h"
#include <QApplication>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#ifdef DARK
#include "ui/AppState.h"
#endif
// Renders every QToolButton of the Layers, Frames and Styles panels, one row per panel, at 1x and in normal + hover-free state.
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Libreworks"));
    QApplication::setOrganizationName(QStringLiteral("Libreworks"));
    theme::apply(app);
#ifdef DARK
    { Prefs p = appState().prefs; p.theme = QStringLiteral("dark"); p.themeFollowSystem = false; appState().setPrefs(p); }
#endif
    QList<QWidget *> panels = {new LayersPanel, new FramesPanel, new StylesPanel};
    QList<QList<QImage>> rows; int W = 0, H = 0;
    for (QWidget *p : panels) {
        p->resize(260, 300); p->show(); app.processEvents();
        QList<QImage> row; int w = 0, h = 0;
        for (QToolButton *b : p->findChildren<QToolButton *>()) {
            if (!b->isVisible() || b->icon().isNull()) continue;
            { const bool en = qEnvironmentVariableIsSet("ENABLED"); if (en) for (QWidget *w = b; w; w = w->parentWidget()) w->setEnabled(true); else b->setEnabled(false); } app.processEvents();
            QImage im = b->grab().toImage().convertToFormat(QImage::Format_ARGB32);
            row << im; w += im.width() + 2; h = std::max(h, im.height());
        }
        rows << row; W = std::max(W, w); H += h + 2;
    }
    QImage out(W, H, QImage::Format_ARGB32); out.fill(Qt::magenta);
    QPainter pt(&out); int y = 0;
    for (auto &row : rows) { int x = 0, h = 0; for (auto &im : row) { pt.drawImage(x, y, im); x += im.width() + 2; h = std::max(h, im.height()); } y += h + 2; }
    pt.end();
    out.save(QString::fromLocal8Bit(argv[1]));
    printf("%dx%d rows:", W, H); for (auto &r : rows) printf(" %d", int(r.size())); printf("\n");
}
