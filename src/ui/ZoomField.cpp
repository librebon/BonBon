#include "AppIcons.h"
#include "ZoomField.h"
#include <QActionGroup>
#include <QKeyEvent>
#include <QMenu>
#include <QStyle>
#include <QRegularExpression>
#include <QEvent>
#include <QTimer>

ZoomField::ZoomField(QWidget *parent) : QLineEdit(parent) {
    setObjectName(QStringLiteral("statusZoom"));
    setToolTip(QStringLiteral("Zoom: click for zoom levels, or type a percentage and press Enter"));
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("00000%")) + 34);
    setFrame(true);
    // a small drop-down arrow at the right edge, like a combo box
    m_arrow = addAction(appicons::standard(QStyle::SP_ArrowDown), QLineEdit::TrailingPosition);
    connect(m_arrow, &QAction::triggered, this, &ZoomField::popup);
    connect(this, &QLineEdit::returnPressed, this, &ZoomField::commitText);
    setZoom(1);
}

const QList<int> &ZoomField::levels() {
    static const QList<int> l{6, 12, 25, 50, 66, 100, 150, 200, 300, 400, 800, 1600, 3200, 6400};
    return l;
}

void ZoomField::setZoom(double z) {
    m_zoom = z;
    if (!hasFocus() || !isModified()) setText(QStringLiteral("%1%").arg(qRound(z * 100)));
    setModified(false);
}

QMenu *ZoomField::buildMenu() {
    auto *m = new QMenu(this);
    m->setObjectName(QStringLiteral("zoomMenu"));
    m->setAttribute(Qt::WA_DeleteOnClose);
    auto *g = new QActionGroup(m);
    const int cur = qRound(m_zoom * 100);
    for (int pct : levels()) {
        QAction *a = m->addAction(QStringLiteral("%1%").arg(pct));
        a->setObjectName(QStringLiteral("zoom%1").arg(pct));
        a->setCheckable(true);
        a->setChecked(pct == cur);
        g->addAction(a);
        connect(a, &QAction::triggered, this, [this, pct] { emit zoomRequested(pct / 100.0); });
    }
    m->addSeparator();
    QAction *fa = m->addAction(QStringLiteral("Fit All"));
    fa->setObjectName(QStringLiteral("zoomFitAll"));
    connect(fa, &QAction::triggered, this, &ZoomField::fitAllRequested);
    QAction *fs = m->addAction(QStringLiteral("Fit Selection"));
    fs->setObjectName(QStringLiteral("zoomFitSelection"));
    connect(fs, &QAction::triggered, this, &ZoomField::fitSelectionRequested);
    return m;
}

void ZoomField::popup() {
    if (!isEnabled()) return;
    QMenu *m = buildMenu();
    // open upwards from the status bar so it stays on screen
    const QSize sh = m->sizeHint();
    m->popup(mapToGlobal(QPoint(width() - sh.width(), -sh.height())));
}

void ZoomField::mousePressEvent(QMouseEvent *e) {
    QLineEdit::mousePressEvent(e);
    if (e->button() != Qt::LeftButton) return;
    // the click both opens the menu and leaves the text selected, ready to type a custom value
    selectAll();
    QTimer::singleShot(0, this, &ZoomField::popup);
}

void ZoomField::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) { setModified(false); setZoom(m_zoom); clearFocus(); return; }
    if (e->key() == Qt::Key_Down && e->modifiers() == Qt::AltModifier) { popup(); return; }
    QLineEdit::keyPressEvent(e);
}

void ZoomField::focusOutEvent(QFocusEvent *e) {
    QLineEdit::focusOutEvent(e);
    setModified(false);
    setZoom(m_zoom);   // abandon an unfinished edit
}

void ZoomField::commitText() {
    static const QRegularExpression num(QStringLiteral("^\\s*([0-9]+(?:[.,][0-9]*)?)\\s*%?\\s*$"));
    const auto mt = num.match(text());
    bool ok = false;
    const double pct = mt.hasMatch() ? mt.captured(1).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok) : 0;
    setModified(false);
    if (ok && pct > 0) emit zoomRequested(qBound(3.0, pct, 6400.0) / 100.0);
    else setZoom(m_zoom);
}

void ZoomField::refreshArrow()
{
    if (m_arrow)
        m_arrow->setIcon(appicons::standard(QStyle::SP_ArrowDown));
}

void ZoomField::changeEvent(QEvent *e)
{
    QLineEdit::changeEvent(e);
    if (!e) return;
    switch (e->type()) {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::ThemeChange:
        refreshArrow();
        break;
    default:
        break;
    }
}
