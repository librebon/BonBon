#include "Theme.h"
#include "DragReorder.h"
#include <QActionGroup>
#include "Panels.h"
#include "Shortcuts.h"
#include "Brand.h"
#include "io/LwdFormat.h"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <memory>
#include "AppState.h"
#include <QTimer>
#include <QScrollBar>
#include <QScrollArea>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include "FileDialogs.h"
#include "PopupSlider.h"
#include "Dialogs.h"
#include "DocumentView.h"
#include "Icons.h"
#include "core/AutoShape.h"
#include <QGridLayout>
#include <QCheckBox>
#include <QApplication>
#include <QButtonGroup>
#include <QMimeData>
#include <QPainter>
#include <QRadioButton>
#include "core/Render.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QHeaderView>
#include <QCache>
#include <QTreeWidgetItemIterator>
#include <QLabel>
#include <QListWidget>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QAbstractItemView>
#include <QStyledItemDelegate>
#include <functional>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QResizeEvent>
#include <algorithm>

namespace {
QToolButton *iconButton(const QString &icon, const QString &tip) {
    auto *b = new QToolButton;
    b->setIcon(icons::named(icon));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFixedSize(24, 22);
    return b;
}
QHBoxLayout *buttonBar(QVBoxLayout *v) {
    auto *w = new QWidget;
    w->setObjectName(QStringLiteral("PanelButtonBar"));
    w->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *bar = new QHBoxLayout(w);
    bar->setContentsMargins(2, 1, 2, 1);
    bar->setSpacing(1);
    v->addWidget(w);
    return bar;
}
// Cap a list/tree so its max height is the space above the button bar (and any
// other chrome), never the full panel. That max is smaller than the previous
// unconstrained height, and the bar cannot overlap the list.
void capListAboveChrome(QWidget *panel, QAbstractItemView *list) {
    if (!panel || !list) return;
    int reserved = 0;
    if (auto *ly = panel->layout()) {
        const auto m = ly->contentsMargins();
        reserved += m.top() + m.bottom();
        reserved += ly->spacing() * qMax(0, ly->count() - 1);
    }
    for (QObject *c : panel->children()) {
        auto *w = qobject_cast<QWidget *>(c);
        if (!w || w == list || !w->isVisible()) continue;
        reserved += qMax(w->minimumSizeHint().height(), w->sizeHint().height());
    }
    const int cap = qMax(list->minimumHeight(), panel->height() - reserved);
    if (list->maximumHeight() != cap)
        list->setMaximumHeight(cap);
}
void relayoutPanelStack(QWidget *w) {
    while (w) {
        if (auto *s = qobject_cast<PanelStack *>(w)) { s->relayout(); return; }
        w = w->parentWidget();
    }
}
}

// ---------------------------------------------------------------- CollapsiblePanel
void CollapsiblePanel::setHeaderText(const QString &t) { m_header->setText(t); }
QString CollapsiblePanel::headerText() const { return m_header->text(); }

CollapsiblePanel::CollapsiblePanel(const QString &title, QWidget *body, QWidget *parent)
    : QWidget(parent), m_body(body), m_title(title) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 2);
    v->setSpacing(0);
    m_header = new QToolButton;
    m_header->setObjectName(QStringLiteral("PanelHeader"));
    m_header->setText(title);
    m_header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_header->setArrowType(Qt::DownArrow);
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_header->setAutoRaise(true);
    v->addWidget(m_header);
    body->setObjectName(QStringLiteral("PanelBody"));
    body->setAttribute(Qt::WA_StyledBackground);
    v->addWidget(body, 1);
    connect(m_header, &QToolButton::clicked, this, [this] { setCollapsed(!m_collapsed); });
}

void CollapsiblePanel::setCollapsed(bool c) {
    if (c == m_collapsed) return;
    m_collapsed = c;
    m_body->setVisible(!c);
    m_header->setArrowType(c ? Qt::RightArrow : Qt::DownArrow);
    setSizePolicy(QSizePolicy::Preferred, (c || m_contentSized) ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    emit collapsedChanged(c);
}

void CollapsiblePanel::setContentSized(bool on) {
    if (on == m_contentSized) return;
    m_contentSized = on;
    if (auto *v = qobject_cast<QVBoxLayout *>(layout()))
        v->setStretchFactor(m_body, on ? 0 : 1);
    if (!m_collapsed)
        setSizePolicy(QSizePolicy::Preferred, on ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    emit collapsedChanged(m_collapsed); // reuse the hook so PanelStack relayouts
}

int CollapsiblePanel::headerHeight() const {
    return qMax(18, m_header->sizeHint().height() + 2);
}

// ---------------------------------------------------------------- Panel splitter + stack

class PanelSplitterHandle : public QWidget {
public:
    explicit PanelSplitterHandle(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("PanelSplitter"));
        setCursor(Qt::SizeVerCursor);
        setFixedHeight(PanelStack::kHandleHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_StyledBackground);
    }
    std::function<void(int)> onDrag;
    std::function<void()> onReleased;
protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            m_lastY = int(e->globalPosition().y());
            e->accept();
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!(e->buttons() & Qt::LeftButton) || !onDrag) return;
        const int y = int(e->globalPosition().y());
        const int dy = y - m_lastY;
        if (dy) {
            m_lastY = y;
            onDrag(dy);
        }
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onReleased) onReleased();
        QWidget::mouseReleaseEvent(e);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        const int mid = height() / 2;
        p.setPen(palette().color(QPalette::Mid));
        p.drawLine(6, mid, width() - 6, mid);
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Dark));
        const int cx = width() / 2;
        for (int i = -2; i <= 2; ++i)
            p.drawEllipse(QPoint(cx + i * 5, mid), 1, 1);
    }
private:
    int m_lastY = 0;
};

PanelScrollArea::PanelScrollArea(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void PanelScrollArea::resizeEvent(QResizeEvent *e)
{
    QScrollArea::resizeEvent(e);
    fitContentsToViewport();
}

bool PanelScrollArea::event(QEvent *e)
{
    const bool ok = QScrollArea::event(e);
    if (e->type() == QEvent::LayoutRequest)
        fitContentsToViewport();
    return ok;
}

bool PanelScrollArea::viewportEvent(QEvent *e)
{
    const bool ok = QScrollArea::viewportEvent(e);
    if (e->type() == QEvent::Resize)
        fitContentsToViewport();
    return ok;
}

void PanelScrollArea::fitContentsToViewport()
{
    QWidget *w = widget();
    if (!w) return;
    const int vw = viewport()->width();
    if (vw <= 0) return;
    // Follow the viewport exactly so a vertical scrollbar never covers the
    // panels. Horizontal scrolling stays off; children reflow to this width.
    if (w->minimumWidth() != vw || w->maximumWidth() != vw || w->width() != vw)
        w->setFixedWidth(vw);
}

PanelStack::PanelStack(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("PanelStack"));
    // Horizontal Ignored: shrink to the scroll viewport when the vertical
    // scrollbar appears. Vertical Minimum: height follows the stacked panels.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addStretch(1);
}

void PanelStack::addPanel(CollapsiblePanel *cp, int defaultStretch) {
    const int insertAt = m_layout->count() - 1; // before the trailing stretch
    if (!m_panels.isEmpty()) {
        auto *h = new PanelSplitterHandle;
        const int idx = m_panels.size() - 1;
        h->onDrag = [this, idx](int dy) {
            if (idx < m_panels.size()) dragBetween(m_panels[idx], dy);
        };
        m_layout->insertWidget(insertAt, h);
        m_handles << h;
    }
    m_layout->insertWidget(m_layout->count() - 1, cp, 0);
    m_panels << cp;
    m_defaultStretch << defaultStretch;
    // stretch 0: size to the panel's content (old QVBoxLayout behaviour). A
    // zero placeholder is filled in applyHeights() once size hints exist.
    const int defH = defaultStretch <= 0 ? 0 : (defaultStretch >= 3 ? 220 : 140);
    m_heights << defH;
    connect(cp, &CollapsiblePanel::collapsedChanged, this, [this] { refreshHandles(); applyHeights(); });
    refreshHandles();
}

static bool layoutItemVisible(QLayoutItem *it)
{
    if (!it) return false;
    if (it->spacerItem()) return false;
    if (QWidget *w = it->widget()) return !w->isHidden();
    if (QLayout *ly = it->layout()) {
        for (int i = 0; i < ly->count(); ++i)
            if (layoutItemVisible(ly->itemAt(i))) return true;
        return false;
    }
    return !it->isEmpty();
}

// Height of a layout from visible items only. QGridLayout is measured by
// visible rows (hidden format rows must not add slack); box layouts follow
// their direction so a grid of Auto Shapes is not summed as a column.
static int tightLayoutHeight(QLayout *ly, int innerWidth)
{
    if (!ly) return 0;
    const auto m = ly->contentsMargins();
    const int inner = innerWidth > 0 ? qMax(1, innerWidth - m.left() - m.right()) : 0;
    int spacing = ly->spacing();
    if (spacing < 0) spacing = 0;

    if (auto *g = qobject_cast<QGridLayout *>(ly)) {
        if (g->verticalSpacing() >= 0) spacing = g->verticalSpacing();
        int h = m.top() + m.bottom();
        int visRows = 0;
        for (int r = 0; r < g->rowCount(); ++r) {
            int rowH = 0;
            bool vis = false;
            QLayoutItem *seen = nullptr;
            for (int c = 0; c < g->columnCount(); ++c) {
                QLayoutItem *it = g->itemAtPosition(r, c);
                if (!it || it == seen || !layoutItemVisible(it)) continue;
                seen = it;
                vis = true;
                int ih = it->sizeHint().height();
                if (it->hasHeightForWidth() && inner > 0)
                    ih = qMax(ih, it->heightForWidth(qMax(1, inner)));
                rowH = qMax(rowH, ih);
            }
            if (!vis) continue;
            h += rowH;
            ++visRows;
        }
        if (visRows > 1) h += spacing * (visRows - 1);
        return h;
    }

    const auto *box = qobject_cast<QBoxLayout *>(ly);
    const bool vertical = !box || box->direction() == QBoxLayout::TopToBottom
                          || box->direction() == QBoxLayout::BottomToTop;
    int h = m.top() + m.bottom();
    int vis = 0;
    int rowH = 0;
    for (int i = 0; i < ly->count(); ++i) {
        QLayoutItem *it = ly->itemAt(i);
        if (!it || !layoutItemVisible(it)) continue;
        int ch = 0;
        if (QWidget *cw = it->widget()) {
            ch = cw->sizeHint().height();
            if (cw->hasHeightForWidth() && inner > 0)
                ch = qMax(ch, cw->heightForWidth(inner));
        } else if (QLayout *sub = it->layout()) {
            ch = tightLayoutHeight(sub, inner);
        } else {
            ch = it->sizeHint().height();
        }
        if (vertical) {
            h += ch;
            ++vis;
        } else {
            rowH = qMax(rowH, ch);
            ++vis;
        }
    }
    if (vertical) {
        if (vis > 1) h += spacing * (vis - 1);
        return h;
    }
    if (vis) h += rowH;
    return h;
}

static int laidOutBodyHeight(QWidget *body)
{
    if (!body) return 0;
    if (auto *ly = body->layout()) {
        ly->activate();
        const int inner = body->width() > 0 ? body->width() : 0;
        const int h = tightLayoutHeight(ly, inner);
        if (h > 0) return h;
    }
    return body->sizeHint().height();
}

int PanelStack::contentHeight(CollapsiblePanel *cp) const
{
    const int header = cp->headerHeight();
    const int bodyH = laidOutBodyHeight(cp->body());
    // CollapsiblePanel layout has a 2px bottom margin.
    return qMax(header + 2, header + bodyH + 2);
}

int PanelStack::minExpandedHeight(CollapsiblePanel *cp) const {
    if (cp->isContentSized())
        return contentHeight(cp);
    // Prefer the body's true minimum (list + button bar) so the bar can never
    // be forced to overlap the list. kMinBody is only a floor for empty stubs.
    int hint = 0;
    if (QWidget *body = cp->body())
        hint = body->minimumSizeHint().height();
    return qMax(cp->headerHeight() + qMax(hint, 48) + 2, cp->minimumSizeHint().height());
}

CollapsiblePanel *PanelStack::nextExpanded(int after) const {
    for (int i = after + 1; i < m_panels.size(); ++i)
        if (!m_panels[i]->isCollapsed()) return m_panels[i];
    return nullptr;
}

int PanelStack::panelHeight(int i) const
{
    if (i < 0 || i >= m_panels.size()) return 0;
    CollapsiblePanel *cp = m_panels[i];
    if (cp->isCollapsed()) return cp->headerHeight();
    if (cp->isContentSized()) return contentHeight(cp);
    return qMax(m_heights[i], minExpandedHeight(cp));
}

int PanelStack::totalHeight() const
{
    int h = 0;
    for (int i = 0; i < m_panels.size(); ++i) {
        h += panelHeight(i);
        if (i < m_handles.size() && m_handles[i]->isVisibleTo(const_cast<PanelStack *>(this)))
            h += kHandleHeight;
    }
    return h;
}

QSize PanelStack::sizeHint() const
{
    int w = 220;
    for (auto *cp : m_panels)
        w = qMax(w, cp->sizeHint().width());
    return QSize(w, totalHeight());
}

QSize PanelStack::minimumSizeHint() const
{
    // Width 0 so the stack can shrink to the viewport once the vertical
    // scrollbar appears. Height is the stacked content so the dock scrolls
    // instead of squashing panels.
    return QSize(0, totalHeight());
}

void PanelStack::relayout()
{
    refreshHandles();
    applyHeights();
}

bool PanelStack::event(QEvent *e)
{
    const bool ok = QWidget::event(e);
    if (e->type() == QEvent::LayoutRequest)
        applyHeights();
    return ok;
}

void PanelStack::refreshHandles() {
    // A handle after a panel resizes that panel (and pushes the ones below).
    // Content-sized panels have no handle: their height follows sizeHint.
    for (int i = 0; i < m_handles.size(); ++i) {
        const bool show = i < m_panels.size() && !m_panels[i]->isCollapsed() && !m_panels[i]->isContentSized();
        m_handles[i]->setVisible(show);
        m_handles[i]->setEnabled(show);
    }
}

int PanelStack::visibleHandleCount() const {
    int n = 0;
    for (auto *h : m_handles)
        if (h->isVisible()) ++n;
    return n;
}

void PanelStack::applyHeights() {
    bool changed = false;
    for (int i = 0; i < m_panels.size(); ++i) {
        CollapsiblePanel *cp = m_panels[i];
        int h;
        if (cp->isCollapsed()) {
            h = cp->headerHeight();
        } else if (cp->isContentSized()) {
            h = contentHeight(cp);
            if (m_heights[i] != h) { m_heights[i] = h; changed = true; }
        } else {
            h = m_heights[i];
            const int mn = minExpandedHeight(cp);
            if (h < mn) {
                if (m_defaultStretch[i] >= 3) h = qMax(mn, 220);
                else if (m_defaultStretch[i] > 0) h = qMax(mn, 140);
                else h = qMax(mn, contentHeight(cp));
                m_heights[i] = h;
                changed = true;
            }
        }
        if (cp->minimumHeight() != h || cp->maximumHeight() != h) {
            cp->setMinimumHeight(h);
            cp->setMaximumHeight(h);
            changed = true;
        }
        m_layout->setStretchFactor(cp, 0);
    }
    const int total = totalHeight();
    if (minimumHeight() != total) {
        setMinimumHeight(total);
        changed = true;
    }
    if (changed)
        updateGeometry();
}

bool PanelStack::dragBetween(CollapsiblePanel *above, int dy) {
    if (!above || above->isCollapsed() || above->isContentSized() || dy == 0) return false;
    const int i = m_panels.indexOf(above);
    if (i < 0) return false;
    const int minA = minExpandedHeight(above);
    const int a = qMax(m_heights[i], minA);
    const int na = qMax(minA, a + dy);
    if (na == a) return false;
    m_heights[i] = na;
    applyHeights();
    return true;
}

int PanelStack::storedHeight(CollapsiblePanel *cp) const {
    const int i = m_panels.indexOf(cp);
    return i >= 0 ? m_heights[i] : 0;
}

void PanelStack::saveLayout(QSettings &st) const {
    st.beginGroup(QStringLiteral("workspace"));
    st.beginGroup(QStringLiteral("rightPanels"));
    st.remove(QString());
    for (int i = 0; i < m_panels.size(); ++i) {
        const QString key = m_panels[i]->title();
        st.setValue(key + QStringLiteral("/height"), m_heights[i]);
        st.setValue(key + QStringLiteral("/collapsed"), m_panels[i]->isCollapsed());
    }
    st.endGroup();
    st.endGroup();
}

void PanelStack::restoreLayout(QSettings &st) {
    st.beginGroup(QStringLiteral("workspace"));
    st.beginGroup(QStringLiteral("rightPanels"));
    for (int i = 0; i < m_panels.size(); ++i) {
        const QString key = m_panels[i]->title();
        if (st.contains(key + QStringLiteral("/height")))
            m_heights[i] = st.value(key + QStringLiteral("/height")).toInt();
        if (st.contains(key + QStringLiteral("/collapsed")))
            m_panels[i]->setCollapsed(st.value(key + QStringLiteral("/collapsed")).toBool());
    }
    st.endGroup();
    st.endGroup();
    refreshHandles();
    applyHeights();
}

// ---------------------------------------------------------------- DocPanel
void DocPanel::setDocument(Document *doc) {
    if (m_doc) disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    if (doc) {
        for (auto sig : {&Document::structureChanged, &Document::frameChanged})
            connect(doc, sig, this, &DocPanel::scheduleRebuild);
        connect(doc, &Document::historyChanged, this, &DocPanel::onHistoryChanged);
        connect(doc, &Document::selectionChanged, this, &DocPanel::scheduleSyncSelection);
    }
    setEnabled(doc != nullptr);
    rebuild();
}

void DocPanel::scheduleRebuild() {
    // coalesce bursts of signals and never rebuild from inside an item view's own signal handler
    if (m_rebuildPending) return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this] { m_rebuildPending = false; rebuild(); });
}

void DocPanel::scheduleSyncSelection() {
    if (m_syncPending) return;
    m_syncPending = true;
    QTimer::singleShot(0, this, [this] { m_syncPending = false; if (!m_rebuildPending) syncSelection(); });   // a rebuild covers it
}

// ---------------------------------------------------------------- LayersPanel
namespace {
enum RowKind { RowLayer = 0, RowObject = 1, RowMask = 2 };
constexpr int KindRole = Qt::UserRole + 1;
constexpr int NameRole = Qt::UserRole + 3;   // the bare name the inline editor starts from (no "(3)" counts)
constexpr int ThumbKeyRole = Qt::UserRole + 4;

// In-place rename (v0.3.4): the editor starts from NameRole, never writes the model itself (the
// row's label carries decorations such as a group's member count), and hands the typed text to
// `commit` after the editor has closed; the panel turns it into one undo step and the rebuild that
// follows refreshes the row. Enter / clicking away commits, Esc cancels (Qt's delegate rules).
class InlineNameDelegate : public QStyledItemDelegate {
public:
    InlineNameDelegate(int column, QObject *parent) : QStyledItemDelegate(parent), m_column(column) {}
    std::function<void(const QModelIndex &, const QString &)> commit;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        if (i.column() != m_column) return QStyledItemDelegate::createEditor(parent, o, i);
        auto *e = new QLineEdit(parent);
        e->setObjectName(QStringLiteral("inlineRename"));
        ++m_open;
        return e;
    }
    void destroyEditor(QWidget *editor, const QModelIndex &i) const override {
        if (qobject_cast<QLineEdit *>(editor) && editor->objectName() == QLatin1String("inlineRename")) m_open = qMax(0, m_open - 1);
        QStyledItemDelegate::destroyEditor(editor, i);
    }
    bool editing() const { return m_open > 0; }
    void setEditorData(QWidget *editor, const QModelIndex &i) const override {
        auto *e = qobject_cast<QLineEdit *>(editor);
        if (!e) { QStyledItemDelegate::setEditorData(editor, i); return; }
        const QVariant n = i.data(NameRole);
        e->setText(n.isValid() ? n.toString() : i.data(Qt::DisplayRole).toString());
        e->selectAll();
    }
    void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &i) const override {
        auto *e = qobject_cast<QLineEdit *>(editor);
        if (e && commit) commit(i, e->text());
    }
private:
    int m_column;
    mutable int m_open = 0;
};

bool inlineEditing(QAbstractItemView *v) {
    auto *d = dynamic_cast<InlineNameDelegate *>(v->itemDelegate());
    return d && d->editing();
}
// maskMode < 0: the object itself; 0 / 1: how it reveals as an alpha (outline) / grayscale mask
QPixmap objectThumb(const VObject &o, const QSize &canvas, int maskMode) {
    const bool asMask = maskMode >= 0;
    QPixmap thumb(20, 16);
    thumb.fill(asMask ? Qt::black : Qt::white);
    VObject copy = o;
    copy.visible = true;
    copy.effects.clear();
    copy.opacity = 100;
    copy.blend = BlendMode::Normal;
    QRect area;
    const QImage full = render::renderObject(copy, &area, QRect(QPoint(0, 0), canvas));
    if (!full.isNull() && !area.isEmpty()) {
        QImage img = full;
        if (asMask) {   // how the mask reveals: white = shown
            img = QImage(full.size(), QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::black);
            QPainter mp(&img);
            QImage w = full.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < w.height(); ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(w.scanLine(y));
                for (int x = 0; x < w.width(); ++x) {
                    const int a = qAlpha(row[x]);
                    const int g = qGray(row[x]);
                    const int v = maskMode == 0 ? a : qMin(a, g);
                    row[x] = qRgba(255, 255, 255, v);
                }
            }
            mp.drawImage(0, 0, w);
        }
        QPainter tp(&thumb);
        QImage sc = img.scaled(18, 14, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        tp.drawImage((20 - sc.width()) / 2, (16 - sc.height()) / 2, sc);
    }
    QPainter tp(&thumb);
    tp.setPen(QColor(0x99, 0x99, 0x99));
    tp.drawRect(0, 0, 19, 15);
    return thumb;
}

QByteArray thumbCacheKey(const VObject &o, const QSize &canvas, int maskMode) {
    VObject c = o;
    c.effects.clear();
    QByteArray k = lwd::objectContentKey(c);
    k += char(0);
    k += QByteArray::number(maskMode);
    k += ':';
    k += QByteArray::number(canvas.width());
    k += 'x';
    k += QByteArray::number(canvas.height());
    return k;
}

QCache<QByteArray, QPixmap> g_layerThumbs(4096);

QPixmap cachedObjectThumb(const VObject &o, const QSize &canvas, int maskMode) {
    const QByteArray key = thumbCacheKey(o, canvas, maskMode);
    if (const QPixmap *hit = g_layerThumbs.object(key)) return *hit;
    QPixmap thumb = objectThumb(o, canvas, maskMode);
    g_layerThumbs.insert(key, new QPixmap(thumb), 1);
    return thumb;
}

void applyThumb(QTreeWidgetItem *it, const VObject &o, const QSize &canvas, int maskMode) {
    const QByteArray key = thumbCacheKey(o, canvas, maskMode);
    if (it->data(2, ThumbKeyRole).toByteArray() == key) return;
    it->setData(2, Qt::DecorationRole, cachedObjectThumb(o, canvas, maskMode));
    it->setData(2, ThumbKeyRole, key);
}
}

LayersPanel::LayersPanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(3);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(kIndent);
    m_tree->setUniformRowHeights(true);
    // v0.3.3: the eye and lock columns stay put at every depth; the indentation and the expand
    // arrow live in the name column (the tree column), which scrolls sideways when deep nesting
    // needs more room than the panel has
    m_tree->setTreePosition(2);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setMinimumSectionSize(kIconCol);
    m_tree->header()->setSectionResizeMode(QHeaderView::Fixed);
    m_tree->setColumnWidth(0, kIconCol);
    m_tree->setColumnWidth(1, kIconCol);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_tree->setObjectName(QStringLiteral("layersTree"));
    m_tree->setMinimumHeight(48);
    m_tree->setMaximumHeight(120);
    m_tree->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_tree->viewport()->installEventFilter(this);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setExpandsOnDoubleClick(false);   // double-clicking a name renames; the arrow expands
    m_tree->installEventFilter(this);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setDragEnabled(true);
    m_tree->setAcceptDrops(true);
    m_tree->setDropIndicatorShown(false);
    m_tree->setDragDropMode(QAbstractItemView::InternalMove);
    m_tree->setDefaultDropAction(Qt::MoveAction);
    m_insert = new DragInsertIndicator(m_tree->viewport());
    connect(&appState(), &AppState::prefsChanged, this, [this] {
        if (!m_doc) return;
        if (appState().lastPrefsChangedWebFeatures()) {
            rebuild();
            return;
        }
        // Theme switches must refresh current-row ink (setBackground caches the
        // first theme); a full rebuild is only needed when web-layer rows appear.
        refreshInPlace();
    });
    {
        auto *dg = new InlineNameDelegate(2, m_tree);
        dg->commit = [this](const QModelIndex &i, const QString &text) {
            const int kind = i.siblingAtColumn(0).data(KindRole).toInt();
            const quint64 id = i.siblingAtColumn(0).data(Qt::UserRole).toULongLong();
            QTimer::singleShot(0, this, [this, kind, id, text] { applyRename(kind, id, text); });
        };
        connect(dg, &QAbstractItemDelegate::closeEditor, this, [this] {
            if (m_rebuildDeferred) { m_rebuildDeferred = false; scheduleRebuild(); }
        });
        m_tree->setItemDelegate(dg);
    }
    v->addWidget(m_tree, 1);
    auto *bar = buttonBar(v);
    auto *add = iconButton(QStringLiteral("layer"), QStringLiteral("New layer"));
    auto *dup = iconButton(QStringLiteral("dup"), QStringLiteral("Duplicate layer"));
    auto *up = iconButton(QStringLiteral("up"), QStringLiteral("Move up"));
    auto *down = iconButton(QStringLiteral("down"), QStringLiteral("Move down"));
    auto *del = iconButton(QStringLiteral("trash"), QStringLiteral("Delete layer or selected objects"));
    del->setObjectName(QStringLiteral("layers.trash"));
    m_share = new QToolButton;
    m_share->setObjectName(QStringLiteral("layerShare"));
    m_share->setText(QStringLiteral("Share"));
    m_share->setCheckable(true);
    m_share->setAutoRaise(true);
    m_share->setToolTip(QStringLiteral("Share this layer across frames"));
    auto *opts = new QToolButton;
    opts->setObjectName(QStringLiteral("layersOptions"));
    opts->setText(QStringLiteral("\u2630"));
    opts->setAutoRaise(true);
    opts->setToolTip(QStringLiteral("Layers options"));
    opts->setPopupMode(QToolButton::InstantPopup);
    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("layersOptionsMenu"));
    QAction *aRename = m_menu->addAction(QStringLiteral("Rename\t") + shortcuts::Manager::get().displayShortcutText(QStringLiteral("panels.rename")), this, [this] { renameCurrent(); });
    aRename->setObjectName(QStringLiteral("layers.rename"));
    // F2 is shown as a hint only: the key is handled by the tree's eventFilter (window F2 toggles the panel)
    m_menu->addSeparator();
    m_menu->addAction(QStringLiteral("New Layer"), add, &QToolButton::click)->setObjectName(QStringLiteral("layers.new"));
    QAction *aDup = m_menu->addAction(QStringLiteral("Duplicate Layer"), dup, &QToolButton::click);
    aDup->setObjectName(QStringLiteral("layers.duplicate"));
    m_menu->addAction(QStringLiteral("Delete"), this, [this] { deleteFromLayers(); })->setObjectName(QStringLiteral("layers.delete"));
    connect(m_menu, &QMenu::aboutToShow, this, [this, aRename, aDup] {
        if (!m_ctxMenu) { m_ctxKind = -1; m_ctxId = 0; m_ctxLayer = -1; }
        m_ctxMenu = false;
        QTreeWidgetItem *it = m_tree->currentItem();
        aRename->setEnabled(m_doc && (!it || (it->flags() & Qt::ItemIsEditable)));
        aDup->setEnabled(m_doc && m_doc->curLayer != m_doc->webLayer());   // the Web Layer is not duplicated
    });
    // Clear the right-click target after the menu is gone. Queued so a triggered
    // Delete still sees m_ctx*; the trash button then uses the selection.
    connect(m_menu, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, [this] {
            m_ctxKind = -1;
            m_ctxId = 0;
            m_ctxLayer = -1;
            m_ctxMenu = false;
        });
    });
    opts->setMenu(m_menu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        // customContextMenuRequested on a QAbstractScrollArea is in viewport coordinates.
        QTreeWidgetItem *it = m_tree->itemAt(pos);
        m_ctxKind = -1;
        m_ctxId = 0;
        m_ctxLayer = -1;
        m_ctxMenu = true;
        if (it) {
            m_ctxKind = it->data(0, KindRole).toInt();
            m_ctxId = it->data(0, Qt::UserRole).toULongLong();
            m_ctxLayer = it->data(1, Qt::UserRole).toInt();
            m_tree->setCurrentItem(it, 2, QItemSelectionModel::NoUpdate);
        }
        m_menu->popup(m_tree->viewport()->mapToGlobal(pos));
    });
    bar->addWidget(opts);
    for (auto *b : {add, dup, up, down}) bar->addWidget(b);
    bar->addWidget(m_share);
    bar->addStretch();
    bar->addWidget(del);
    connect(m_share, &QToolButton::clicked, this, [this](bool on) {
        if (!m_doc || m_doc->curLayer < 0 || m_doc->curLayer >= m_doc->s.layers.size()) return;
        if (m_doc->s.layers[m_doc->curLayer].web) { m_share->setChecked(true); return; }
        if (m_doc->s.layers[m_doc->curLayer].shared == on) return;
        m_doc->setLayerShared(m_doc->curLayer, on);
        m_doc->commit(on ? QStringLiteral("Share Layer") : QStringLiteral("Unshare Layer"));
    });

    connect(add, &QToolButton::clicked, this, [this] {
        if (!m_doc) return;
        m_doc->addLayer();
        m_doc->commit(QStringLiteral("New Layer"));
    });
    connect(dup, &QToolButton::clicked, this, [this] {
        if (!m_doc || m_doc->duplicateLayer(m_doc->curLayer) < 0) return;   // not the Web Layer
        m_doc->commit(QStringLiteral("Duplicate Layer"));
    });
    connect(up, &QToolButton::clicked, this, [this] { moveCurrent(1); });
    connect(down, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
    connect(del, &QToolButton::clicked, this, [this] { deleteFromLayers(); });
    connect(m_tree, &QTreeWidget::itemClicked, this, &LayersPanel::onClicked);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *it) {
        fitNameColumn();
        if (m_building) return;
        const quint64 id = it->data(0, Qt::UserRole).toULongLong();
        if (it->parent()) m_openGroups.remove(id); else m_collapsedLayers.insert(id);
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) {
        fitNameColumn();
        if (m_building) return;
        const quint64 id = it->data(0, Qt::UserRole).toULongLong();
        if (it->parent()) m_openGroups.insert(id); else m_collapsedLayers.remove(id);
    });
    setDocument(nullptr);
}

bool LayersPanel::eventFilter(QObject *o, QEvent *e) {
    if (o == m_tree->viewport() && e->type() == QEvent::Resize) fitNameColumn();
    if (o == m_tree->viewport() && e->type() == QEvent::MouseButtonDblClick) {
        // a double-click on the name (not the eye / lock columns, not the expand arrow) renames
        auto *me = static_cast<QMouseEvent *>(e);
        const QModelIndex idx = m_tree->indexAt(me->position().toPoint());
        if (me->button() == Qt::LeftButton && idx.isValid() && idx.column() == 2 && me->position().x() >= m_tree->visualRect(idx).left()) {
            if (startRename(m_tree->itemFromIndex(idx))) return true;
        }
    }
    // Rename shortcut (default F2) wins over window.layers when the tree is focused
    if (o == m_tree && !inlineEditing(m_tree)
        && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && shortcuts::Manager::get().matches(QStringLiteral("panels.rename"), static_cast<QKeyEvent *>(e))) {
        if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
        renameCurrent();
        return true;
    }
    // Let Arrange shortcuts (Ctrl+Up/Down etc.) reach the window actions; don't
    // let the tree use those keys to change the current row.
    if (o == m_tree && !inlineEditing(m_tree)
        && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)) {
        auto *ke = static_cast<QKeyEvent *>(e);
        auto &sm = shortcuts::Manager::get();
        if (sm.matches(QStringLiteral("arrange.front"), ke) || sm.matches(QStringLiteral("arrange.forward"), ke)
            || sm.matches(QStringLiteral("arrange.backward"), ke) || sm.matches(QStringLiteral("arrange.back"), ke)) {
            if (e->type() == QEvent::ShortcutOverride) { e->ignore(); return false; }
            e->accept();
            return true;
        }
    }
    if (o == m_tree->viewport() && m_doc && m_insert) {
        if (e->type() == QEvent::DragEnter) {
            auto *de = static_cast<QDragEnterEvent *>(e);
            de->acceptProposedAction();
            return true;
        }
        if (e->type() == QEvent::DragMove) {
            auto *de = static_cast<QDragMoveEvent *>(e);
            updateLayerInsertIndicator(de->position().toPoint());
            if (m_insert->active()) de->acceptProposedAction();
            else de->ignore();
            return true;
        }
        if (e->type() == QEvent::DragLeave) {
            m_insert->clear();
            return true;
        }
        if (e->type() == QEvent::Drop) {
            auto *de = static_cast<QDropEvent *>(e);
            const bool ok = applyLayersDrop(de->position().toPoint());
            m_insert->clear();
            if (ok) de->acceptProposedAction();
            else de->ignore();
            return true;
        }
    }
    return DocPanel::eventFilter(o, e);
}

bool LayersPanel::startRename(QTreeWidgetItem *it) {
    if (!m_doc || !it || !(it->flags() & Qt::ItemIsEditable)) return false;
    m_tree->setCurrentItem(it, 2, QItemSelectionModel::NoUpdate);
    m_tree->editItem(it, 2);
    return true;
}

void LayersPanel::renameCurrent() {
    if (!m_doc) return;
    QTreeWidgetItem *it = m_tree->currentItem();
    if (!it) {   // nothing current: the current layer's row
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
            if (m_tree->topLevelItem(i)->data(1, Qt::UserRole).toInt() == m_doc->curLayer) { it = m_tree->topLevelItem(i); break; }
    }
    startRename(it);
}

// One undo step per rename; empty / whitespace-only names and the Web Layer are left alone.
void LayersPanel::applyRename(int kind, quint64 id, const QString &text) {
    if (!m_doc) return;
    const QString n = text.trimmed();
    if (n.isEmpty()) { scheduleRebuild(); return; }
    if (kind == RowLayer) {
        for (auto &l : m_doc->s.layers)
            if (l.id == id && !l.web && l.name != n) { l.name = n; m_doc->commit(QStringLiteral("Rename Layer")); return; }
    } else if (VObject *o = m_doc->find(id)) {
        if (o->name == n || (o->name.isEmpty() && o->displayName() == n)) return;
        o->name = n;
        m_doc->commit(QStringLiteral("Rename Object"));
    }
}

// The name column fills the panel, but never gets narrower than the deepest open row needs for
// its indentation, arrow, thumbnail and a readable (elided) name; beyond that the tree scrolls.
void LayersPanel::fitNameColumn() {
    int depth = 0;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        int d = 0;
        bool shown = true;
        for (QTreeWidgetItem *p = (*it)->parent(); p; p = p->parent()) { ++d; if (!p->isExpanded()) shown = false; }
        if (shown) depth = qMax(depth, d);
    }
    const int need = (depth + 1) * m_tree->indentation() + m_tree->iconSize().width() + kThumbW + kMinNameText;
    const int avail = m_tree->viewport()->width() - m_tree->columnWidth(0) - m_tree->columnWidth(1);
    const int w = qMax(avail, need);
    if (m_tree->columnWidth(2) != w) m_tree->setColumnWidth(2, w);
}

void LayersPanel::deleteFromLayers()
{
    if (!m_doc) return;
    const int kind = m_ctxKind;
    const quint64 id = m_ctxId;
    const int layer = m_ctxLayer;
    m_ctxKind = -1;
    m_ctxId = 0;
    m_ctxLayer = -1;
    if (kind == RowObject || kind == RowMask) {
        // Clicked row is in the selection (including multi-select): delete the selection,
        // the same as canvas Delete. Otherwise delete only the clicked object, even when
        // something else is selected or the selection is empty.
        if (m_doc->selection.contains(id)) {
            m_doc->deleteSelection();
        } else {
            const QSet<quint64> rest = m_doc->selection - QSet<quint64>{id};
            m_doc->selectOnly(id);
            m_doc->deleteSelection();
            m_doc->setSelection(rest);
        }
        m_doc->commit(QStringLiteral("Delete"));
        return;
    }
    if (kind == RowLayer) {
        if (m_doc->s.layers.size() <= 1) return;
        const int web = m_doc->webLayer();
        if (web >= 0 && layer == web) return;
        m_doc->deleteLayer(layer);
        m_doc->commit(QStringLiteral("Delete Layer"));
        return;
    }
    if (!m_doc->selection.isEmpty()) { m_doc->deleteSelection(); m_doc->commit(QStringLiteral("Delete")); return; }
    if (m_doc->s.layers.size() <= 1) return;
    m_doc->deleteLayer(m_doc->curLayer);
    m_doc->commit(QStringLiteral("Delete Layer"));
}

void LayersPanel::onClicked(QTreeWidgetItem *it, int col) {
    if (!m_doc || !it) return;
    const bool isLayer = !it->parent();
    const quint64 id = it->data(0, Qt::UserRole).toULongLong();
    const int layer = it->data(1, Qt::UserRole).toInt();
    if (it->data(0, KindRole).toInt() == RowMask && (col == 0 || col == 1)) {
        // the mask row's eye enables / disables the mask, its second column links / unlinks it
        if (VObject *g = m_doc->find(it->data(0, Qt::UserRole + 2).toULongLong())) {
            if (col == 0) g->maskEnabled = !g->maskEnabled; else g->maskLinked = !g->maskLinked;
            m_doc->commit(col == 0 ? (g->maskEnabled ? QStringLiteral("Enable Mask") : QStringLiteral("Disable Mask"))
                                   : (g->maskLinked ? QStringLiteral("Link Mask") : QStringLiteral("Unlink Mask")));
        }
        return;
    }
    if (col == 0 || col == 1) {
        if (isLayer) {
            LayerInfo &li = m_doc->s.layers[layer];
            if (col == 0) li.visible = !li.visible; else li.locked = !li.locked;
            m_doc->commit(col == 0 ? (li.visible ? QStringLiteral("Show Layer") : QStringLiteral("Hide Layer"))
                                   : (li.locked ? QStringLiteral("Lock Layer") : QStringLiteral("Unlock Layer")));
        } else if (VObject *o = m_doc->find(id)) {
            if (col == 0) o->visible = !o->visible; else o->locked = !o->locked;
            m_doc->commit(col == 0 ? (o->visible ? QStringLiteral("Show Object") : QStringLiteral("Hide Object"))
                                   : (o->locked ? QStringLiteral("Lock Object") : QStringLiteral("Unlock Object")));
        }
        return;
    }
    m_doc->curLayer = layer;
    if (isLayer) {
        m_doc->clearSelection();
        scheduleRebuild();
    } else {
        QSet<quint64> sel;
        for (QTreeWidgetItem *s : m_tree->selectedItems())
            if (s->parent()) sel.insert(s->data(0, Qt::UserRole).toULongLong());
        if (sel.isEmpty()) sel.insert(id);
        // picking a group member (or a mask) works inside its group, as after a double-click on the canvas
        VObject *pg = m_doc->parentGroup(id);
        if (pg) m_doc->enterGroup(pg->id); else m_doc->enteredGroup = 0;
        m_doc->setSelection(sel);
    }
}

void LayersPanel::addObjectItem(QTreeWidgetItem *parent, const VObject &o, int layer) {
    const QIcon eye = icons::named(QStringLiteral("eye")), lock = icons::named(QStringLiteral("lock"));
    auto *c = new QTreeWidgetItem(parent);
    c->setData(0, Qt::UserRole, o.id);
    c->setData(1, Qt::UserRole, layer);
    c->setData(0, KindRole, int(RowObject));
    if (o.visible) c->setIcon(0, eye);
    if (o.locked) c->setIcon(1, lock);
    QString label = o.displayName();
    if (o.type == ObjType::Group) label += QStringLiteral(" (%1)").arg(o.children.size());
    c->setText(2, label);
    c->setData(2, NameRole, o.displayName());
    c->setFlags(c->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    SymbolScope symScope(&m_doc->s.symbols);
    applyThumb(c, o, QSize(m_doc->s.width, m_doc->s.height), -1);
    if (m_doc->selection.contains(o.id)) c->setSelected(true);
    if (o.type != ObjType::Group) return;
    if (m_doc->enteredGroup == o.id) {
        QFont fi = c->font(2);
        fi.setItalic(true);
        c->setFont(2, fi);
        c->setToolTip(2, QStringLiteral("Entered group (Esc on the canvas leaves it)"));
    }
    // members top first, then the mask row (Fireworks shows the mask beside the group)
    for (int i = o.children.size() - 1; i >= 0; --i) addObjectItem(c, o.children[i], layer);
    if (o.hasMask()) {
        const VObject &m = o.maskObj[0];
        auto *mr = new QTreeWidgetItem(c);
        mr->setData(0, Qt::UserRole, m.id);
        mr->setData(1, Qt::UserRole, layer);
        mr->setData(0, KindRole, int(RowMask));
        mr->setData(0, Qt::UserRole + 2, o.id);
        if (o.maskEnabled) mr->setIcon(0, eye);
        if (o.maskLinked) mr->setIcon(1, icons::named(QStringLiteral("link")));
        const QString mode = o.maskMode == 1 ? QStringLiteral("Grayscale") : QStringLiteral("Alpha / Outline");
        const QString state = QStringLiteral("%1%2").arg(mode, o.maskEnabled ? QString() : QStringLiteral(", disabled"));
        mr->setText(2, m.name.isEmpty() ? QStringLiteral("Mask: %1").arg(state) : QStringLiteral("%1 (Mask: %2)").arg(m.name, state));
        mr->setData(2, NameRole, m.name.isEmpty() ? QStringLiteral("Mask") : m.name);
        mr->setFlags((mr->flags() | Qt::ItemIsEditable) & ~Qt::ItemIsDragEnabled);
        mr->setToolTip(0, QStringLiteral("Enable / disable the mask"));
        mr->setToolTip(1, QStringLiteral("Link: the mask moves with the group"));
        applyThumb(mr, m, QSize(m_doc->s.width, m_doc->s.height), o.maskMode);
        if (m_doc->selection.contains(m.id)) mr->setSelected(true);
    }
    bool open = m_openGroups.contains(o.id) || m_doc->enteredGroup == o.id;
    if (!open) for (quint64 id : m_doc->selection) if (id != o.id && m_doc->isNestedIn(id, o.id)) { open = true; break; }
    c->setExpanded(open);
}

bool LayersPanel::refreshInPlace() {
    if (!m_doc || !m_tree || m_tree->topLevelItemCount() == 0) return false;
    if (m_doc->curFrame != m_builtFrame || m_doc->s.frames.isEmpty()) return false;
    if (m_tree->topLevelItemCount() != m_doc->s.layers.size()) return false;
    const Frame &f = m_doc->s.frames[m_doc->curFrame];
    const QSize canvas(m_doc->s.width, m_doc->s.height);
    const QIcon eye = icons::named(QStringLiteral("eye")), lock = icons::named(QStringLiteral("lock"));
    auto updateMask = [&](QTreeWidgetItem *mr, const VObject &grp) -> bool {
        if (!grp.hasMask()) return false;
        const VObject &m = grp.maskObj[0];
        if (mr->data(0, KindRole).toInt() != RowMask) return false;
        if (mr->data(0, Qt::UserRole).toULongLong() != m.id) return false;
        mr->setData(1, Qt::UserRole, mr->data(1, Qt::UserRole));
        mr->setData(0, Qt::UserRole + 2, grp.id);
        if (grp.maskEnabled) mr->setIcon(0, eye); else mr->setIcon(0, QIcon());
        if (grp.maskLinked) mr->setIcon(1, icons::named(QStringLiteral("link"))); else mr->setIcon(1, QIcon());
        const QString mode = grp.maskMode == 1 ? QStringLiteral("Grayscale") : QStringLiteral("Alpha / Outline");
        const QString state = QStringLiteral("%1%2").arg(mode, grp.maskEnabled ? QString() : QStringLiteral(", disabled"));
        mr->setText(2, m.name.isEmpty() ? QStringLiteral("Mask: %1").arg(state) : QStringLiteral("%1 (Mask: %2)").arg(m.name, state));
        mr->setData(2, NameRole, m.name.isEmpty() ? QStringLiteral("Mask") : m.name);
        applyThumb(mr, m, canvas, grp.maskMode);
        mr->setSelected(m_doc->selection.contains(m.id));
        return true;
    };
    std::function<bool(QTreeWidgetItem *, const VObject &, int)> updateObj =
        [&](QTreeWidgetItem *c, const VObject &o, int layer) -> bool {
        if (c->data(0, KindRole).toInt() != RowObject) return false;
        if (c->data(0, Qt::UserRole).toULongLong() != o.id) return false;
        c->setData(1, Qt::UserRole, layer);
        if (o.visible) c->setIcon(0, eye); else c->setIcon(0, QIcon());
        if (o.locked) c->setIcon(1, lock); else c->setIcon(1, QIcon());
        QString label = o.displayName();
        if (o.type == ObjType::Group) label += QStringLiteral(" (%1)").arg(o.children.size());
        c->setText(2, label);
        c->setData(2, NameRole, o.displayName());
        applyThumb(c, o, canvas, -1);
        c->setSelected(m_doc->selection.contains(o.id));
        if (o.type == ObjType::Group) {
            QFont fi = c->font(2);
            fi.setItalic(m_doc->enteredGroup == o.id);
            c->setFont(2, fi);
            const int mask = o.hasMask() ? 1 : 0;
            if (c->childCount() != o.children.size() + mask) return false;
            for (int i = 0; i < o.children.size(); ++i) {
                const VObject &ch = o.children[o.children.size() - 1 - i];
                if (!updateObj(c->child(i), ch, layer)) return false;
            }
            if (o.hasMask() && !updateMask(c->child(c->childCount() - 1), o)) return false;
            bool open = m_openGroups.contains(o.id) || m_doc->enteredGroup == o.id;
            if (!open) for (quint64 id : m_doc->selection) if (id != o.id && m_doc->isNestedIn(id, o.id)) { open = true; break; }
            c->setExpanded(open);
        } else if (c->childCount() != 0) {
            return false;
        }
        return true;
    };
    for (int ti = 0; ti < m_tree->topLevelItemCount(); ++ti) {
        QTreeWidgetItem *it = m_tree->topLevelItem(ti);
        const int li = m_doc->s.layers.size() - 1 - ti;
        const LayerInfo &L = m_doc->s.layers[li];
        if (it->data(0, KindRole).toInt() != RowLayer) return false;
        if (it->data(0, Qt::UserRole).toULongLong() != L.id) return false;
        it->setData(1, Qt::UserRole, li);
        if (L.visible) it->setIcon(0, eye); else it->setIcon(0, QIcon());
        if (L.locked) it->setIcon(1, lock); else it->setIcon(1, QIcon());
        it->setText(2, L.name);
        it->setData(2, NameRole, L.name);
        QFont lf = m_tree->font();
        lf.setBold(true);
        lf.setItalic(L.shared);
        it->setFont(2, lf);
        if (li == m_doc->curLayer) {
            // A custom background disables QSS selected-item text, so set the
            // theme ink explicitly; otherwise a switch leaves the first theme's
            // cached item foreground (dark text on a dark current-row).
            const QColor ink = theme::color("text");
            for (int c = 0; c < 3; ++c) {
                it->setBackground(c, theme::color("currentRow"));
                it->setForeground(c, ink);
            }
        } else {
            for (int c = 0; c < 3; ++c) {
                it->setBackground(c, QBrush());
                it->setForeground(c, QBrush());
            }
        }
        const int nObj = (li < f.layers.size()) ? f.layers[li].size() : 0;
        if (it->childCount() != nObj) return false;
        for (int ci = 0; ci < nObj; ++ci) {
            const VObject &o = f.layers[li][nObj - 1 - ci];
            if (!updateObj(it->child(ci), o, li)) return false;
        }
        it->setExpanded(!m_collapsedLayers.contains(L.id));
    }
    if (m_share) {
        m_share->setEnabled(true);
        if (m_doc->curLayer >= 0 && m_doc->curLayer < m_doc->s.layers.size()) {
            const LayerInfo &cur = m_doc->s.layers[m_doc->curLayer];
            m_share->setChecked(cur.shared);
            m_share->setEnabled(!cur.web);
        }
    }
    return true;
}

void LayersPanel::onHistoryChanged() {
    // A single-object edit keeps the tree shape: update rows and thumbs in place.
    // structureChanged / frameChanged still go through scheduleRebuild (full rebuild).
    if (inlineEditing(m_tree)) { m_rebuildDeferred = true; return; }
    if (refreshInPlace()) {
        m_builtLayer = m_doc->curLayer;
        m_builtFrame = m_doc->curFrame;
        m_builtGroup = m_doc->enteredGroup;
        fitNameColumn();
        return;
    }
    scheduleRebuild();
}

void LayersPanel::rebuild() {
    // never tear the tree down under the inline editor: rebuild once it closes
    if (inlineEditing(m_tree)) { m_rebuildDeferred = true; return; }
    m_building = true;
    // keep the scroll position and the current row across the rebuild (a click on a row must not
    // scroll the list under the pointer: the second click of a double-click lands on the same row)
    const int vScroll = m_tree->verticalScrollBar()->value(), hScroll = m_tree->horizontalScrollBar()->value();
    int curKind = -1;
    quint64 curId = 0;
    if (QTreeWidgetItem *c = m_tree->currentItem()) { curKind = c->data(0, KindRole).toInt(); curId = c->data(0, Qt::UserRole).toULongLong(); }
    m_tree->clear();
    m_share->setEnabled(m_doc != nullptr);
    if (!m_doc) { m_building = false; return; }
    if (m_doc->curLayer >= 0 && m_doc->curLayer < m_doc->s.layers.size()) {
        const LayerInfo &cur = m_doc->s.layers[m_doc->curLayer];
        m_share->setChecked(cur.shared);
        m_share->setEnabled(!cur.web);
    }
    const QIcon eye = icons::named(QStringLiteral("eye")), lock = icons::named(QStringLiteral("lock"));
    const Frame &f = m_doc->s.frames[m_doc->curFrame];
    QFont bold = m_tree->font();
    bold.setBold(true);
    for (int li = m_doc->s.layers.size() - 1; li >= 0; --li) {
        const LayerInfo &L = m_doc->s.layers[li];
        auto *it = new QTreeWidgetItem(m_tree);
        it->setData(0, Qt::UserRole, L.id);
        it->setData(1, Qt::UserRole, li);
        if (L.visible) it->setIcon(0, eye);
        if (L.locked) it->setIcon(1, lock);
        it->setText(2, L.name);
        it->setData(0, KindRole, int(RowLayer));
        it->setData(2, NameRole, L.name);
        if (!L.web) it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        else it->setFlags(it->flags() & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
        QFont lf = bold;
        lf.setItalic(L.shared);
        it->setFont(2, lf);
        if (L.shared) it->setToolTip(2, L.web ? (webFeaturesEnabled()
                                                ? QStringLiteral("Web Layer (slices and hotspots, shared across frames)")
                                                : QStringLiteral("Web Layer (slices, shared across frames)"))
                                              : QStringLiteral("Shared across frames"));
        if (li == m_doc->curLayer) {
            const QColor ink = theme::color("text");
            for (int c = 0; c < 3; ++c) {
                it->setBackground(c, theme::color("currentRow"));
                it->setForeground(c, ink);
            }
        }
        if (li < f.layers.size()) {
            for (int oi = f.layers[li].size() - 1; oi >= 0; --oi) {
                const VObject &o = f.layers[li][oi];
                if (o.type == ObjType::Hotspot && !webFeaturesEnabled()) continue;
                addObjectItem(it, o, li);
            }
        }
        it->setExpanded(!m_collapsedLayers.contains(L.id));
    }
    if (curKind >= 0) {
        for (QTreeWidgetItemIterator iter(m_tree); *iter; ++iter)
            if ((*iter)->data(0, KindRole).toInt() == curKind && (*iter)->data(0, Qt::UserRole).toULongLong() == curId) {
                m_tree->setCurrentItem(*iter, 2, QItemSelectionModel::NoUpdate);
                break;
            }
    }
    m_builtLayer = m_doc->curLayer;
    m_builtFrame = m_doc->curFrame;
    m_builtGroup = m_doc->enteredGroup;
    m_building = false;
    fitNameColumn();
    m_tree->verticalScrollBar()->setValue(vScroll);
    m_tree->horizontalScrollBar()->setValue(hScroll);
}

void LayersPanel::syncSelection() {
    if (inlineEditing(m_tree)) { m_rebuildDeferred = true; return; }
    if (!m_doc || m_doc->curLayer != m_builtLayer || m_doc->curFrame != m_builtFrame || m_doc->enteredGroup != m_builtGroup) { rebuild(); return; }
    // the same rows rebuild() would select and the same groups it would open, set on the existing tree
    QHash<quint64, QTreeWidgetItem *> rows;
    QVector<QTreeWidgetItem *> groups;
    for (QTreeWidgetItemIterator iter(m_tree); *iter; ++iter) {
        QTreeWidgetItem *it = *iter;
        const int kind = it->data(0, KindRole).toInt();
        if (kind == RowLayer) continue;
        rows.insert(it->data(0, Qt::UserRole).toULongLong(), it);
        if (kind == RowObject && it->childCount()) groups << it;
    }
    QItemSelection sel;
    for (quint64 id : std::as_const(m_doc->selection)) {
        QTreeWidgetItem *it = rows.value(id);
        if (!it) { rebuild(); return; }   // the tree is out of date: build it again
        const QModelIndex i = m_tree->indexFromItem(it, 0);
        sel.select(i, i);
    }
    m_building = true;
    m_tree->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    for (QTreeWidgetItem *g : std::as_const(groups)) {
        const quint64 gid = g->data(0, Qt::UserRole).toULongLong();
        bool open = m_openGroups.contains(gid) || m_doc->enteredGroup == gid;
        if (!open) for (quint64 id : std::as_const(m_doc->selection)) if (id != gid && m_doc->isNestedIn(id, gid)) { open = true; break; }
        if (g->isExpanded() != open) g->setExpanded(open);
    }
    m_building = false;
}

void LayersPanel::moveCurrent(int dir)
{
    if (!m_doc || dir == 0) return;
    QTreeWidgetItem *it = m_tree->currentItem();
    const int kind = it ? it->data(0, KindRole).toInt() : RowLayer;
    if (it && (kind == RowObject) && it->parent()) {
        const quint64 id = it->data(0, Qt::UserRole).toULongLong();
        if (!m_doc->selection.contains(id)) m_doc->selectOnly(id);
        // Tree lists top-first: up is bring forward (how = 1), down is send backward (how = 2).
        m_doc->arrange(dir > 0 ? 1 : 2);
        m_doc->commit(dir > 0 ? QStringLiteral("Bring Forward") : QStringLiteral("Send Backward"));
        return;
    }
    if (dir > 0) {
        if (m_doc->curLayer + 1 >= m_doc->s.layers.size()) return;
        const int web = m_doc->webLayer();
        if (web >= 0 && (m_doc->curLayer == web || m_doc->curLayer + 1 >= web)) return;
        m_doc->moveLayer(m_doc->curLayer, m_doc->curLayer + 1);
        m_doc->commit(QStringLiteral("Move Layer"));
    } else {
        if (m_doc->curLayer <= 0 || m_doc->curLayer == m_doc->webLayer()) return;
        m_doc->moveLayer(m_doc->curLayer, m_doc->curLayer - 1);
        m_doc->commit(QStringLiteral("Move Layer"));
    }
}

namespace {
QVector<QTreeWidgetItem *> draggedLayerItems(QTreeWidget *tree) {
    QVector<QTreeWidgetItem *> out;
    for (QTreeWidgetItem *s : tree->selectedItems()) {
        if (s->data(0, KindRole).toInt() == RowLayer) out.append(s);
    }
    return out;
}

QVector<quint64> draggedObjectIds(QTreeWidget *tree, Document *doc) {
    QVector<quint64> ids;
    QSet<quint64> seen;
    for (QTreeWidgetItem *s : tree->selectedItems()) {
        const int kind = s->data(0, KindRole).toInt();
        if (kind != RowObject) continue;
        const quint64 id = s->data(0, Qt::UserRole).toULongLong();
        if (seen.contains(id) || !doc->find(id)) continue;
        // Skip members whose ancestor is also selected (the group moves as a whole).
        bool nested = false;
        for (QTreeWidgetItem *p = s->parent(); p; p = p->parent()) {
            if (p->data(0, KindRole).toInt() == RowObject && tree->selectedItems().contains(p)) { nested = true; break; }
        }
        if (nested) continue;
        seen.insert(id);
        ids.append(id);
    }
    return ids;
}

// Storage index if we insert above (dir=-1) or below (dir=+1) this tree item among its siblings.
// Siblings are listed top-first, storage is back-to-front.
int storageIndexForDrop(QTreeWidgetItem *target, bool above, bool isLayer)
{
    QTreeWidgetItem *parent = target ? target->parent() : nullptr;
    int n = 0, row = 0;
    if (isLayer) {
        QTreeWidget *tree = target->treeWidget();
        n = tree->topLevelItemCount();
        row = tree->indexOfTopLevelItem(target);
    } else {
        n = parent ? parent->childCount() : 0;
        row = parent ? parent->indexOfChild(target) : 0;
        // skip mask rows at the end when counting objects? masks are last; they are not objects
        int objN = 0, objRow = 0;
        if (parent) {
            for (int c = 0; c < parent->childCount(); ++c) {
                if (parent->child(c)->data(0, KindRole).toInt() == RowMask) continue;
                if (parent->child(c) == target) objRow = objN;
                ++objN;
            }
            n = objN;
            row = objRow;
        }
    }
    // tree row 0 (top) = storage n-1; drop above row r -> storage n-r; drop below row r -> n-1-r
    if (above) return n - row;
    return n - 1 - row;
}
}

void LayersPanel::updateLayerInsertIndicator(const QPoint &viewportPos)
{
    if (!m_doc || !m_insert) { if (m_insert) m_insert->clear(); return; }
    QTreeWidgetItem *over = m_tree->itemAt(viewportPos);
    const auto layerItems = draggedLayerItems(m_tree);
    const auto objIds = draggedObjectIds(m_tree, m_doc);
    const bool dragLayers = !layerItems.isEmpty() && objIds.isEmpty();
    const bool dragObjects = !objIds.isEmpty() && layerItems.isEmpty();
    if (!dragLayers && !dragObjects) { m_insert->clear(); return; }

    if (!over) {
        // empty space at the bottom of the list: end of the last (bottom) layer
        if (dragLayers) {
            // below the bottom-most (lowest index) layer, but not under a web-only drop
            if (m_tree->topLevelItemCount() == 0) { m_insert->clear(); return; }
            QTreeWidgetItem *last = m_tree->topLevelItem(m_tree->topLevelItemCount() - 1);
            const int li = last->data(1, Qt::UserRole).toInt();
            if (m_doc->s.layers.value(li).web) { m_insert->clear(); return; }
            const QRect vr = m_tree->visualItemRect(last);
            m_insert->showLine(QRect(0, vr.bottom() - 1, m_tree->viewport()->width(), 2));
            return;
        }
        if (m_tree->topLevelItemCount() == 0) { m_insert->clear(); return; }
        QTreeWidgetItem *last = m_tree->topLevelItem(m_tree->topLevelItemCount() - 1);
        const QRect vr = m_tree->visualItemRect(last);
        m_insert->showLine(QRect(0, vr.bottom() - 1, m_tree->viewport()->width(), 2));
        return;
    }
    const int kind = over->data(0, KindRole).toInt();
    const QRect vr = m_tree->visualItemRect(over);
    const int mid = vr.center().y();
    const bool above = viewportPos.y() < mid;
    const int vw = m_tree->viewport()->width();

    if (dragLayers) {
        if (kind != RowLayer) { m_insert->clear(); return; }
        const int li = over->data(1, Qt::UserRole).toInt();
        if (m_doc->s.layers.value(li).web) { m_insert->clear(); return; }
        m_insert->showLine(QRect(0, (above ? vr.top() : vr.bottom()) - 1, vw, 2));
        return;
    }

    // objects
    if (kind == RowLayer) {
        const int li = over->data(1, Qt::UserRole).toInt();
        if (m_doc->s.layers.value(li).web || m_doc->s.layers.value(li).locked) { m_insert->clear(); return; }
        // In the middle of the layer row: drop into the layer (front). Near the edges: still into.
        m_insert->showInto(QRect(0, vr.top(), vw, vr.height()));
        return;
    }
    if (kind == RowMask) { m_insert->clear(); return; }
    if (kind == RowObject) {
        VObject *overObj = m_doc->find(over->data(0, Qt::UserRole).toULongLong());
        const bool isGroup = overObj && overObj->type == ObjType::Group;
        const int band = vr.height() / 3;
        const bool onto = isGroup && viewportPos.y() >= vr.top() + band && viewportPos.y() <= vr.bottom() - band;
        if (onto) {
            if (overObj->locked) { m_insert->clear(); return; }
            m_insert->showInto(QRect(0, vr.top(), vw, vr.height()));
            return;
        }
        m_insert->showLine(QRect(0, (above ? vr.top() : vr.bottom()) - 1, vw, 2));
        return;
    }
    m_insert->clear();
}

bool LayersPanel::applyLayersDrop(const QPoint &viewportPos)
{
    if (!m_doc) return false;
    QTreeWidgetItem *over = m_tree->itemAt(viewportPos);
    const auto layerItems = draggedLayerItems(m_tree);
    const auto objIds = draggedObjectIds(m_tree, m_doc);
    const bool dragLayers = !layerItems.isEmpty() && objIds.isEmpty();
    const bool dragObjects = !objIds.isEmpty() && layerItems.isEmpty();
    if (!dragLayers && !dragObjects) return false;

    if (dragLayers) {
        if (!over || over->data(0, KindRole).toInt() != RowLayer) {
            if (!over && m_tree->topLevelItemCount() > 0)
                over = m_tree->topLevelItem(m_tree->topLevelItemCount() - 1);
            else return false;
        }
        const int destLi = over->data(1, Qt::UserRole).toInt();
        if (m_doc->s.layers.value(destLi).web) return false;
        const QRect vr = m_tree->visualItemRect(over);
        const bool above = over && viewportPos.y() < vr.center().y();
        int dest = storageIndexForDrop(over, above, true);
        // dest is a storage index in the layer list. Top-first tree: storage index = n-1-row.
        // storageIndexForDrop already returns the insertion index in storage.
        // Moving one layer: use moveLayer for each, from current to dest, high to low carefully.
        // Simpler: move the first selected layer (Fireworks moves the current layer).
        QVector<int> from;
        for (QTreeWidgetItem *it : layerItems) {
            const int li = it->data(1, Qt::UserRole).toInt();
            if (li == m_doc->webLayer()) continue;
            from.append(li);
        }
        if (from.isEmpty()) return false;
        if (!m_doc->moveLayers(from, dest)) return false;
        m_doc->commit(QStringLiteral("Move Layer"));
        return true;
    }

    // objects
    int destLayer = -1;
    quint64 destGroup = 0;
    int destIndex = 0;
    // Empty space below the list: the insertion line sits under the last (bottom)
    // layer, so objects go to the back of that layer (storage index 0).
    if (!over) {
        if (m_tree->topLevelItemCount() == 0) return false;
        QTreeWidgetItem *last = m_tree->topLevelItem(m_tree->topLevelItemCount() - 1);
        destLayer = last->data(1, Qt::UserRole).toInt();
        if (m_doc->s.layers.value(destLayer).web || m_doc->s.layers.value(destLayer).locked) return false;
        destGroup = 0;
        destIndex = 0;
        if (!m_doc->relocateObjects(objIds, destLayer, destGroup, destIndex)) return false;
        m_doc->commit(QStringLiteral("Move Objects"));
        return true;
    }
    QTreeWidgetItem *target = over;
    int kind = target->data(0, KindRole).toInt();
    const QRect vr = m_tree->visualItemRect(target);
    const bool above = viewportPos.y() < vr.center().y();

    if (kind == RowLayer) {
        destLayer = target->data(1, Qt::UserRole).toInt();
        if (m_doc->s.layers.value(destLayer).web || m_doc->s.layers.value(destLayer).locked) return false;
        destGroup = 0;
        destIndex = m_doc->objects(destLayer).size(); // front of the layer
    } else if (kind == RowObject) {
        destLayer = target->data(1, Qt::UserRole).toInt();
        if (m_doc->s.layers.value(destLayer).web || m_doc->s.layers.value(destLayer).locked) return false;
        VObject *o = m_doc->find(target->data(0, Qt::UserRole).toULongLong());
        const bool isGroup = o && o->type == ObjType::Group;
        const int band = vr.height() / 3;
        const bool onto = isGroup && viewportPos.y() >= vr.top() + band && viewportPos.y() <= vr.bottom() - band;
        if (onto) {
            if (o->locked) return false;
            destGroup = o->id;
            destIndex = o->children.size();
        } else {
            VObject *pg = m_doc->parentGroup(target->data(0, Qt::UserRole).toULongLong());
            destGroup = pg ? pg->id : 0;
            destIndex = storageIndexForDrop(target, above, false);
        }
    } else {
        return false;
    }
    if (!m_doc->relocateObjects(objIds, destLayer, destGroup, destIndex)) return false;
    m_doc->commit(QStringLiteral("Move Objects"));
    return true;
}


void LayersPanel::resizeEvent(QResizeEvent *e) {
    DocPanel::resizeEvent(e);
    capListAboveChrome(this, m_tree);
}

// ---------------------------------------------------------------- FramesPanel
FramesPanel::FramesPanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("framesList"));
    m_list->setMinimumHeight(48);
    m_list->setMaximumHeight(64);
    m_list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    v->addWidget(m_list, 1);
    auto *optsW = new QWidget;
    optsW->setObjectName(QStringLiteral("framesOpts"));
    optsW->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *opts = new QHBoxLayout(optsW);
    opts->setContentsMargins(4, 2, 4, 2);
    m_delay = new PopupSliderField;
    m_delay->setRange(10, 60000);
    m_delay->setSingleStep(10);
    m_delay->setSuffix(QStringLiteral(" ms"));
    m_delay->setKeyboardTracking(false);
    m_delay->setToolTip(QStringLiteral("Frame delay"));
    static_cast<PopupSliderField *>(m_delay)->setSliderRange(10, 2000);
    m_loops = new PopupSliderField;
    m_loops->setRange(0, 1000);
    m_loops->setSpecialValueText(QStringLiteral("Forever"));
    m_loops->setPrefix(QStringLiteral("Loops: "));
    m_loops->setKeyboardTracking(false);
    static_cast<PopupSliderField *>(m_loops)->setSliderRange(0, 20);
    m_loops->setToolTip(QStringLiteral("Number of times the animation plays (0 = forever)"));
    opts->addWidget(new QLabel(QStringLiteral("Delay:")));
    opts->addWidget(m_delay);
    opts->addWidget(m_loops);
    v->addWidget(optsW);
    auto *bar = buttonBar(v);
    auto *prev = iconButton(QStringLiteral("prev"), QStringLiteral("Previous frame"));
    m_play = iconButton(QStringLiteral("play"), QStringLiteral("Play / stop"));
    m_play->setObjectName(QStringLiteral("framesPlay"));
    auto *next = iconButton(QStringLiteral("next"), QStringLiteral("Next frame"));
    auto *add = iconButton(QStringLiteral("frame"), QStringLiteral("New frame"));
    auto *dup = iconButton(QStringLiteral("dup"), QStringLiteral("Duplicate frame"));
    auto *del = iconButton(QStringLiteral("trash"), QStringLiteral("Delete frame"));
    for (auto *b : {prev, m_play, next}) bar->addWidget(b);
    bar->addStretch();
    m_onion = new QToolButton;
    m_onion->setObjectName(QStringLiteral("framesOnion"));
    m_onion->setText(QStringLiteral("Onion"));
    m_onion->setToolTip(QStringLiteral("Onion skinning"));
    m_onion->setAutoRaise(true);
    m_onion->setCheckable(true);
    m_onion->setPopupMode(QToolButton::InstantPopup);
    {
        auto *menu = new QMenu(m_onion);
        menu->setObjectName(QStringLiteral("onionMenu"));
        const QStringList modes = {QStringLiteral("No Onion Skinning"), QStringLiteral("Show Next Frame"),
                                   QStringLiteral("Before and After"), QStringLiteral("Show All Frames")};
        auto *grp = new QActionGroup(menu);
        for (int i = 0; i < modes.size(); ++i) {
            QAction *a = menu->addAction(modes[i]);
            a->setCheckable(true);
            a->setData(i);
            grp->addAction(a);
            connect(a, &QAction::triggered, this, [this, i] { if (m_doc) { m_doc->setOnion(i); rebuild(); } });
        }
        menu->addSeparator();
        connect(menu->addAction(QStringLiteral("Opacity...")), &QAction::triggered, this, [this] {
            if (!m_doc) return;
            bool ok = false;
            const int opacity = QInputDialog::getInt(this, QStringLiteral("Onion Skinning"), QStringLiteral("Opacity (%):"), m_doc->onionOpacity, 1, 100, 5, &ok);
            if (ok) m_doc->setOnion(m_doc->onionMode, opacity);
        });
        connect(menu, &QMenu::aboutToShow, this, [this, grp] {
            for (QAction *a : grp->actions()) a->setChecked(m_doc && a->data().toInt() == m_doc->onionMode);
        });
        m_onion->setMenu(menu);
    }
    bar->addWidget(m_onion);
    for (auto *b : {add, dup, del}) bar->addWidget(b);

    connect(prev, &QToolButton::clicked, this, [this] { if (m_doc) m_doc->setCurrentFrame((m_doc->curFrame + m_doc->s.frames.size() - 1) % m_doc->s.frames.size()); });
    connect(next, &QToolButton::clicked, this, [this] { if (m_doc) m_doc->setCurrentFrame((m_doc->curFrame + 1) % m_doc->s.frames.size()); });
    connect(m_play, &QToolButton::clicked, this, [this] { play(!m_timer.isActive()); });
    connect(add, &QToolButton::clicked, this, [this] { if (m_doc) { m_doc->addFrame(false); m_doc->commit(QStringLiteral("Add Frame")); } });
    connect(dup, &QToolButton::clicked, this, [this] { if (m_doc) { m_doc->addFrame(true); m_doc->commit(QStringLiteral("Duplicate Frame")); } });
    connect(del, &QToolButton::clicked, this, [this] {
        if (m_doc && m_doc->s.frames.size() > 1) { m_doc->deleteFrame(m_doc->curFrame); m_doc->commit(QStringLiteral("Delete Frame")); }
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int r) { if (!m_building && m_doc && r >= 0) m_doc->setCurrentFrame(r); });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_doc) return;
        QMenu menu;
        menu.setObjectName(QStringLiteral("framesContextMenu"));
        QAction *copy = menu.addAction(QStringLiteral("Copy to Frames..."));
        QAction *dist = menu.addAction(QStringLiteral("Distribute to Frames"));
        const bool hasSel = !m_doc->selection.isEmpty();
        copy->setEnabled(hasSel && m_doc->s.frames.size() >= 1);
        dist->setEnabled(hasSel);
        QAction *picked = menu.exec(m_list->mapToGlobal(pos));
        if (!picked || !m_doc) return;
        if (picked == copy) {
            CopyToFramesDialog dlg(m_doc->s.frames.size(), m_doc->curFrame, this);
            if (dlg.exec() != QDialog::Accepted) return;
            const QVector<int> t = dlg.targets();
            if (t.isEmpty()) return;
            m_doc->copyToFrames(t);
            m_doc->commit(QStringLiteral("Copy to Frames"));
        } else if (picked == dist) {
            m_doc->distributeToFrames();
            m_doc->commit(QStringLiteral("Distribute to Frames"));
        }
    });
    // in-place rename (v0.3.4): double-click or F2; one "Rename Frame" undo step
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->installEventFilter(this);
    {
        auto *dg = new InlineNameDelegate(0, m_list);
        dg->commit = [this](const QModelIndex &i, const QString &text) {
            const int row = i.row();
            QTimer::singleShot(0, this, [this, row, text] {
                const QString n = text.trimmed();
                if (!m_doc || row < 0 || row >= m_doc->s.frames.size() || n.isEmpty() || m_doc->s.frames[row].name == n
                    || (m_doc->s.frames[row].name.isEmpty() && n == QStringLiteral("Frame %1").arg(row + 1))) { scheduleRebuild(); return; }
                m_doc->s.frames[row].name = n;
                m_doc->commit(QStringLiteral("Rename Frame"));
            });
        };
        connect(dg, &QAbstractItemDelegate::closeEditor, this, [this] {
            if (m_rebuildDeferred) { m_rebuildDeferred = false; scheduleRebuild(); }
        });
        m_list->setItemDelegate(dg);
    }
    m_list->viewport()->installEventFilter(this);   // double-click renames (see eventFilter)
    connect(m_delay, &QSpinBox::valueChanged, this, [this](int ms) {
        if (m_building || !m_doc || m_doc->frame().delayMs == ms) return;
        m_doc->frame().delayMs = ms;
        m_doc->commit(QStringLiteral("Frame Delay"));
    });
    connect(m_loops, &QSpinBox::valueChanged, this, [this](int n) {
        if (m_building || !m_doc || m_doc->s.loops == n) return;
        m_doc->s.loops = n;
        m_doc->commit(QStringLiteral("Animation Loops"));
    });
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_doc || m_doc->s.frames.size() < 2) { play(false); return; }
        m_doc->setCurrentFrame((m_doc->curFrame + 1) % m_doc->s.frames.size());
        m_timer.start(m_doc->frame().delayMs);
    });
    m_thumbTimer.setSingleShot(true);
    m_thumbTimer.setInterval(0);
    connect(&m_thumbTimer, &QTimer::timeout, this, &FramesPanel::fillThumbs);
    setDocument(nullptr);
}

void FramesPanel::setDocument(Document *doc) {
    play(false);
    DocPanel::setDocument(doc);
}

void FramesPanel::renameCurrent() {
    if (!m_doc || !m_list->currentItem()) return;
    m_list->editItem(m_list->currentItem());
}

bool FramesPanel::eventFilter(QObject *o, QEvent *e) {
    // handled here rather than via itemDoubleClicked: the first click switches frames and rebuilds
    // the list, which would otherwise swallow the double-click
    if (o == m_list->viewport() && e->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::LeftButton) {
            if (QListWidgetItem *it = m_list->itemAt(me->position().toPoint())) {
                m_list->setCurrentItem(it);
                renameCurrent();
                return true;
            }
        }
    }
    // Rename shortcut (default F2) when the frames list is focused
    if (o == m_list && !inlineEditing(m_list)
        && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && shortcuts::Manager::get().matches(QStringLiteral("panels.rename"), static_cast<QKeyEvent *>(e))) {
        if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
        renameCurrent();
        return true;
    }
    return DocPanel::eventFilter(o, e);
}

void FramesPanel::play(bool on) {
    if (on && m_doc && m_doc->s.frames.size() > 1) m_timer.start(m_doc->frame().delayMs);
    else m_timer.stop();
    m_play->setIcon(icons::named(m_timer.isActive() ? QStringLiteral("stop") : QStringLiteral("play")));
}

void FramesPanel::rebuild() {
    if (inlineEditing(m_list)) { m_rebuildDeferred = true; return; }
    m_building = true;
    m_list->clear();
    if (m_doc) {
        for (int i = 0; i < m_doc->s.frames.size(); ++i) {
            const Frame &f = m_doc->s.frames[i];
            const QString name = f.name.isEmpty() ? QStringLiteral("Frame %1").arg(i + 1) : f.name;
            auto *it = new QListWidgetItem(QStringLiteral("%1   %2 ms").arg(name).arg(f.delayMs), m_list);
            it->setData(NameRole, name);
            it->setFlags(it->flags() | Qt::ItemIsEditable);
            if (i < m_thumbs.size()) it->setIcon(m_thumbs[i].pix);   // until fillThumbs() brings it up to date
        }
        m_list->setIconSize(QSize(32, 24));
        fillThumbs();   // renders only frames whose thumbnail is out of date, and only while the panel is visible
        m_list->setCurrentRow(m_doc->curFrame);
        m_delay->setValue(m_doc->frame().delayMs);
        m_loops->setValue(m_doc->s.loops);
    }
    m_onion->setChecked(m_doc && m_doc->onionMode > 0);
    m_building = false;
}

void FramesPanel::showEvent(QShowEvent *e) {
    DocPanel::showEvent(e);
    m_thumbTimer.start();   // thumbnails are only rendered while the panel is visible: catch up now
}

void FramesPanel::fillThumbs() {
    if (!m_doc || !isVisible() || inlineEditing(m_list)) return;
    const int n = int(m_doc->s.frames.size());
    QVector<Thumb> next(n);
    QVector<int> order;
    order.reserve(n);
    if (m_doc->curFrame >= 0 && m_doc->curFrame < n) order << m_doc->curFrame;   // the current frame first
    for (int i = 0; i < n; ++i) if (i != m_doc->curFrame) order << i;
    for (int i : std::as_const(order)) {
        next[i].key = lwd::contentKey(m_doc->s, i);
        for (const Thumb &t : std::as_const(m_thumbs))
            if (t.key == next[i].key) { next[i].pix = t.pix; break; }
        if (next[i].pix.isNull()) {
            next[i].pix = QPixmap::fromImage(m_doc->render(i, true).scaled(32, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            ++m_thumbRenders;
        }
    }
    m_thumbs = next;
    if (m_list->count() != n) return;   // a rebuild is on its way and will set the icons
    const bool was = m_building;
    m_building = true;   // setIcon() emits itemChanged
    for (int i = 0; i < n; ++i)
        if (m_list->item(i)->icon().cacheKey() != m_thumbs[i].pix.cacheKey()) m_list->item(i)->setIcon(m_thumbs[i].pix);
    m_building = was;
}

void FramesPanel::resizeEvent(QResizeEvent *e) {
    DocPanel::resizeEvent(e);
    capListAboveChrome(this, m_list);
}

// ---------------------------------------------------------------- HistoryPanel
HistoryPanel::HistoryPanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("historyList"));
    m_list->setMinimumHeight(48);
    m_list->setMaximumHeight(72);
    m_list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    v->addWidget(m_list, 1);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int r) {
        if (!m_building && m_doc && r >= 0) m_doc->jumpTo(r);
    });
    setDocument(nullptr);
}

void HistoryPanel::resizeEvent(QResizeEvent *e) {
    DocPanel::resizeEvent(e);
    capListAboveChrome(this, m_list);
}

void HistoryPanel::rebuild() {
    m_building = true;
    m_list->clear();
    if (m_doc) {
        const auto &h = m_doc->history();
        for (int i = 0; i < h.size(); ++i) {
            auto *it = new QListWidgetItem(h[i].label, m_list);
            if (i > m_doc->historyIndex()) it->setForeground(theme::color("disabled"));
        }
        m_list->setCurrentRow(m_doc->historyIndex());
        m_list->scrollToItem(m_list->currentItem());
    }
    m_building = false;
}

// ---------------------------------------------------------------- OptimizePanel
static constexpr int kOptionsDelay = 400;   // ms after a settings change
static constexpr int kEditDelay = 1000;     // ms after the last document edit
OptimizePanel::OptimizePanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    m_w = new OptimizeWidget;
    m_w->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    v->addWidget(m_w, 0);
    auto *bar = buttonBar(v);
    auto *exp = new QToolButton;
    exp->setText(QStringLiteral("Export..."));
    exp->setToolTip(QStringLiteral("Export with these settings (Ctrl+Shift+R)"));
    bar->addStretch();
    bar->addWidget(exp);
    connect(exp, &QToolButton::clicked, this, &OptimizePanel::exportRequested);
    connect(m_w, &OptimizeWidget::optionsChanged, this, [this](const ExportOptions &o) {
        if (!m_doc) return;
        const bool changed = !(m_doc->exportOptions == o);
        m_doc->exportOptions = o;
        if (changed) m_doc->markViewDirty();
        emit optionsChanged();
        m_estimate.start(kOptionsDelay);
        relayoutPanelStack(this);
    });
    connect(m_w, &OptimizeWidget::optimizeToSizeRequested, this, [this] {
        if (!m_doc) return;
        ExportOptions o = m_doc->exportOptions;
        if (!runOptimizeToSizeWizard(this, *m_doc, o, m_doc->curFrame)) return;
        m_doc->exportOptions = o;
        m_doc->markViewDirty();
        m_w->setOptions(o);
        emit optionsChanged();
        m_estimate.start(kOptionsDelay);
    });
    m_estimate.setSingleShot(true);
    m_estimate.setInterval(kOptionsDelay);
    connect(&m_estimate, &QTimer::timeout, this, &OptimizePanel::estimate);
    m_watch = new QFutureWatcher<EncodeResult>(this);
    connect(m_watch, &QFutureWatcherBase::finished, this, &OptimizePanel::estimateDone);
    setDocument(nullptr);
}

static int optimizePanelHeightForWidth(const OptimizePanel *p, int w)
{
    int h = 0;
    int spacing = 0, mt = 0, mb = 0;
    if (auto *ly = p->layout()) {
        spacing = ly->spacing();
        if (spacing < 0) spacing = 0;
        mt = ly->contentsMargins().top();
        mb = ly->contentsMargins().bottom();
    }
    if (OptimizeWidget *ow = p->widget()) {
        const int iw = w > 0 ? w : (p->width() > 0 ? p->width() : 250);
        int oh = ow->sizeHint().height();
        if (ow->hasHeightForWidth())
            oh = qMax(oh, ow->heightForWidth(qMax(1, iw)));
        h += oh;
    }
    if (auto *bar = p->findChild<QWidget *>(QStringLiteral("PanelButtonBar")))
        h += bar->sizeHint().height();
    return h + mt + mb + spacing;
}

QSize OptimizePanel::sizeHint() const
{
    return QSize(160, optimizePanelHeightForWidth(this, width() > 0 ? width() : 250));
}

QSize OptimizePanel::minimumSizeHint() const
{
    return QSize(120, optimizePanelHeightForWidth(this, width() > 0 ? width() : 120));
}

int OptimizePanel::heightForWidth(int w) const
{
    return optimizePanelHeightForWidth(this, w);
}

void OptimizePanel::setDocument(Document *doc) {
    if (m_doc) disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    setEnabled(doc != nullptr);
    // an edit restarts the wait, so a drag or a run of edits is estimated once, after it settles
    if (doc) connect(doc, &Document::changed, this, [this] { m_estimate.start(kEditDelay); });
    rebuild();
}

void OptimizePanel::rebuild() {
    if (!m_doc) return;
    m_w->setOptions(m_doc->exportOptions);
    m_estimate.start(kOptionsDelay);
    relayoutPanelStack(this);
}

void OptimizePanel::showEvent(QShowEvent *e) {
    DocPanel::showEvent(e);
    if (m_estimateStale) { m_estimateStale = false; m_estimate.start(kOptionsDelay); }
}

bool OptimizePanel::estimatePending() const { return m_estimate.isActive() || m_watch->isRunning(); }

void OptimizePanel::estimate() {
    if (!m_doc || !isVisible()) { m_w->setSizeText({}); m_estimateStale = m_doc; return; }
    if (m_watch->isRunning()) { m_estimateAgain = true; return; }   // one at a time; the newest state follows
    // the full render and the encode run on a worker thread over a snapshot, so the UI never waits
    std::shared_ptr<const Document> snap(m_doc->snapshot());
    m_estimateDoc = m_doc;
    m_watch->setFuture(QtConcurrent::run([snap] { return encodePreview(*snap, snap->exportOptions, snap->curFrame); }));
}

void OptimizePanel::estimateDone() {
    const EncodeResult r = m_watch->result();
    ++m_estimatesRun;
    const bool again = m_estimateAgain;
    m_estimateAgain = false;
    if (m_estimateDoc != m_doc || !m_doc) { if (m_doc) estimate(); return; }   // another document by now
    if (again) estimate();
    const QString saving = describeSaving(r);
    m_w->setSizeText(r.error.isEmpty() ? QStringLiteral("Estimated: %1").arg(describeSize(r.bytes)) + (saving.isEmpty() ? QString() : QStringLiteral("\n") + saving) : r.error);
    m_w->setColorTable(r.error.isEmpty() ? r.colorTable : QVector<QRgb>());
    relayoutPanelStack(this);
}

// ---------------------------------------------------------------- AlignPanel
namespace align {
bool apply(Document *doc, int how, bool toCanvas) {
    if (!doc) return false;
    auto objs = doc->selectedEditable();
    if (objs.isEmpty() || (!toCanvas && objs.size() < 2 && how < 6)) {
        if (objs.isEmpty()) return false;
        toCanvas = true;
    }
    QRectF ref = toCanvas ? QRectF(0, 0, doc->s.width, doc->s.height) : doc->selectionBounds();
    if (how <= 5) {
        for (VObject *o : objs) {
            QRectF b = o->bounds();
            QPointF d;
            switch (how) {
            case 0: d.setX(ref.left() - b.left()); break;
            case 1: d.setX(ref.center().x() - b.center().x()); break;
            case 2: d.setX(ref.right() - b.right()); break;
            case 3: d.setY(ref.top() - b.top()); break;
            case 4: d.setY(ref.center().y() - b.center().y()); break;
            case 5: d.setY(ref.bottom() - b.bottom()); break;
            }
            o->moveBy(d);
        }
    } else if (how <= 7) {
        if (objs.size() < 3 && !toCanvas) return false;
        const bool horiz = how == 6;
        std::sort(objs.begin(), objs.end(), [horiz](VObject *a, VObject *b) {
            return horiz ? a->bounds().center().x() < b->bounds().center().x() : a->bounds().center().y() < b->bounds().center().y();
        });
        double total = 0;
        for (VObject *o : objs) total += horiz ? o->bounds().width() : o->bounds().height();
        double span = horiz ? ref.width() : ref.height();
        double gap = objs.size() > 1 ? (span - total) / (objs.size() - (toCanvas ? -1 : 1)) : 0;
        double pos = (horiz ? ref.left() : ref.top()) + (toCanvas ? gap : 0);
        for (VObject *o : objs) {
            QRectF b = o->bounds();
            if (horiz) { o->moveBy({pos - b.left(), 0}); pos += b.width() + gap; }
            else { o->moveBy({0, pos - b.top()}); pos += b.height() + gap; }
        }
    } else {
        QRectF first = objs.first()->bounds();
        for (VObject *o : objs) {
            QRectF b = o->bounds();
            if (how == 8) b.setWidth(toCanvas ? ref.width() : first.width());
            else b.setHeight(toCanvas ? ref.height() : first.height());
            o->setBounds(b);
        }
    }
    static const char *labels[] = {"Align Left", "Align Center Vertical Axis", "Align Right", "Align Top", "Align Center Horizontal Axis",
                                   "Align Bottom", "Distribute Widths", "Distribute Heights", "Make Same Width", "Make Same Height"};
    Q_ASSERT(how >= 0 && how < int(std::size(labels)));
    doc->commit(how >= 0 && how < int(std::size(labels)) ? QString::fromLatin1(labels[how]) : QStringLiteral("Align"));
    return true;
}
}

AlignPanel::AlignPanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(4, 2, 4, 2);
    v->setSpacing(2);
    static const char *names[] = {"L", "C", "R", "T", "M", "B", "|↔|", "↕", "=W", "=H"};
    static const char *tips[] = {"Align left", "Align centre (vertical axis)", "Align right", "Align top", "Align middle (horizontal axis)",
                                 "Align bottom", "Distribute horizontally", "Distribute vertically", "Make same width", "Make same height"};
    auto makeBtn = [](int i) {
        auto *b = new QToolButton;
        b->setText(QString::fromUtf8(names[i]));
        b->setToolTip(QString::fromLatin1(tips[i]));
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedHeight(22);
        // Width follows the label. A shared 28px cap (used when this panel was
        // condensed) is narrower than "|↔|" plus the theme's 2px padding and
        // 1px border, so QToolButton elided it to "...".
        b->ensurePolished();
        const int tw = b->fontMetrics().horizontalAdvance(b->text());
        // A couple of extra pixels so the trailing "|" of "|↔|" is not
        // pressed against the 1px border (which made it look clipped).
        b->setFixedWidth(qMax(24, qMax(b->sizeHint().width() + 4, tw + 14)));
        return b;
    };
    auto *row1 = new QHBoxLayout;
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(2);
    for (int i = 0; i < 6; ++i) {
        auto *b = makeBtn(i);
        row1->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, i] { align(i); });
    }
    v->addLayout(row1);
    auto *row2 = new QHBoxLayout;
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(2);
    for (int i = 6; i < 10; ++i) {
        auto *b = makeBtn(i);
        row2->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, i] { align(i); });
    }
    row2->addStretch(1);
    m_toCanvas = new QToolButton;
    m_toCanvas->setObjectName(QStringLiteral("alignToCanvas"));
    m_toCanvas->setCheckable(true);
    m_toCanvas->setAutoRaise(true);
    m_toCanvas->setFocusPolicy(Qt::NoFocus);
    m_toCanvas->setIcon(icons::named(QStringLiteral("canvas")));
    m_toCanvas->setIconSize(QSize(16, 16));
    m_toCanvas->setFixedSize(24, 22);
    m_toCanvas->setToolTip(QStringLiteral("Align relative to the canvas instead of the selection"));
    row2->addWidget(m_toCanvas, 0, Qt::AlignRight | Qt::AlignVCenter);
    v->addLayout(row2);
    setDocument(nullptr);
}

void AlignPanel::align(int how) { align::apply(m_doc, how, m_toCanvas->isChecked()); }

// ---------------------------------------------------------------- StylesPanel
StylesPanel::StylesPanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("stylesList"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(44, 44));
    m_list->setGridSize(QSize(52, 52));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);   // Static + InternalMove: a drop reorders the rows (not free positioning)
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setMinimumHeight(52);
    m_list->setMaximumHeight(QWIDGETSIZE_MAX);
    m_list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setToolTip(QStringLiteral("Click to select, double-click to apply to the selected objects. Drag to reorder."));
    m_list->installEventFilter(this);
    v->addWidget(m_list, 1);
    QHBoxLayout *bar = buttonBar(v);
    m_opts = new QToolButton;
    m_opts->setObjectName(QStringLiteral("stylesOptions"));
    m_opts->setText(QStringLiteral("\u2630"));
    m_opts->setAutoRaise(true);
    m_opts->setToolTip(QStringLiteral("Styles options: new, rename, reorder, delete, import, export, reset"));
    m_opts->setPopupMode(QToolButton::InstantPopup);
    bar->addWidget(m_opts);
    bar->addStretch();
    m_new = iconButton(QStringLiteral("dup"), QStringLiteral("New Style: from the selected object, or from the current fill and stroke colours"));
    m_new->setObjectName(QStringLiteral("styleNew"));
    m_del = iconButton(QStringLiteral("trash"), QStringLiteral("Delete the selected styles (Delete)"));
    m_del->setObjectName(QStringLiteral("styleDelete"));
    bar->addWidget(m_new);
    bar->addWidget(m_del);

    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("stylesOptionsMenu"));
    auto add = [this](const char *obj, const QString &text, auto fn) {
        QAction *a = m_menu->addAction(text, this, fn);
        a->setObjectName(QString::fromLatin1(obj));
        return a;
    };
    QAction *aApply = add("styles.apply", QStringLiteral("Apply to Selection"), [this] { if (!selectedRows().isEmpty()) applyStyle(selectedRows().first()); });
    m_menu->addSeparator();
    add("styles.new", QStringLiteral("New Style..."), [this] { newStyleDialog(); });
    QAction *aDup = add("styles.duplicate", QStringLiteral("Duplicate Style"), [this] { if (!selectedRows().isEmpty()) duplicateStyle(selectedRows().first()); });
    QAction *aRen = add("styles.rename", QStringLiteral("Rename Style..."), [this] { if (!selectedRows().isEmpty()) renameDialog(selectedRows().first()); });
    QAction *aDel = add("styles.delete", QStringLiteral("Delete Styles"), [this] { deleteSelectedAsk(); });
    m_menu->addSeparator();
    QAction *aUp = add("styles.moveEarlier", QStringLiteral("Move Earlier"), [this] { const auto r = selectedRows(); if (!r.isEmpty()) moveStyle(r.first(), r.first() - 1); });
    QAction *aDown = add("styles.moveLater", QStringLiteral("Move Later"), [this] { const auto r = selectedRows(); if (!r.isEmpty()) moveStyle(r.first(), r.first() + 1); });
    m_menu->addSeparator();
    add("styles.import", QStringLiteral("Import Styles..."), [this] {
        const QString f = filedialogs::getOpenFileName(this, QStringLiteral("Import Styles"), QString(), QStringLiteral("%1 styles (*.lwstyles *.json)").arg(brand::name()));
        if (f.isEmpty()) return;
        QString err;
        if (importStyles(f, &err) <= 0) QMessageBox::warning(this, QStringLiteral("Import Styles"), QStringLiteral("Could not import %1: %2").arg(f, err));
    });
    QAction *aExp = add("styles.export", QStringLiteral("Export Styles..."), [this] {
        QString f = filedialogs::getSaveFileName(this, QStringLiteral("Export Styles"), QStringLiteral("styles.lwstyles"), QStringLiteral("%1 styles (*.lwstyles)").arg(brand::name()));
        if (f.isEmpty()) return;
        if (!f.endsWith(QLatin1String(".lwstyles"))) f += QStringLiteral(".lwstyles");
        QString err;
        if (!exportStyles(f, selectedRows(), &err)) QMessageBox::warning(this, QStringLiteral("Export Styles"), QStringLiteral("Could not export: %1").arg(err));
    });
    m_menu->addSeparator();
    add("styles.reset", QStringLiteral("Reset Styles..."), [this] {
        if (!qApp->property("lw_testing").toBool()
            && QMessageBox::question(this, QStringLiteral("Reset Styles"), QStringLiteral("Restore the built-in styles (deleted, renamed or moved ones are put back)? Your own styles are kept.")) != QMessageBox::Yes)
            return;
        resetStyles();
    });
    connect(m_menu, &QMenu::aboutToShow, this, [=, this] {
        const auto r = selectedRows();
        aApply->setEnabled(!r.isEmpty() && m_doc && !m_doc->selection.isEmpty());
        aDup->setEnabled(r.size() == 1);
        aRen->setEnabled(r.size() == 1);
        aDel->setEnabled(!r.isEmpty());
        aUp->setEnabled(r.size() == 1 && r.first() > 0);
        aDown->setEnabled(r.size() == 1 && r.first() < m_styles.size() - 1);
        aExp->setText(r.isEmpty() ? QStringLiteral("Export Styles...") : QStringLiteral("Export Selected Styles..."));
        aExp->setEnabled(!m_styles.isEmpty());
    });
    m_opts->setMenu(m_menu);

    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) { applyStyle(m_list->row(it)); });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] { rebuild(); });
    connect(m_new, &QToolButton::clicked, this, &StylesPanel::newStyleDialog);
    connect(m_del, &QToolButton::clicked, this, &StylesPanel::deleteSelectedAsk);
    // drag-reorder: the list widget moves its rows; follow it
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, [this] { syncOrderFromList(); });
    connect(m_list->model(), &QAbstractItemModel::rowsInserted, this, [this] { QTimer::singleShot(0, this, [this] { syncOrderFromList(); }); });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (QListWidgetItem *it = m_list->itemAt(pos); it && !it->isSelected()) {
            m_list->clearSelection();
            it->setSelected(true);
            m_list->setCurrentItem(it);
        }
        m_menu->exec(m_list->viewport()->mapToGlobal(pos));
    });
    reload();
    setDocument(nullptr);
    setEnabled(true); // the list is usable without a document; applying needs one
}

bool StylesPanel::eventFilter(QObject *o, QEvent *e) {
    if (o == m_list && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) { deleteSelectedAsk(); return true; }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (QListWidgetItem *it = m_list->currentItem()) applyStyle(m_list->row(it));
            return true;
        }
        if (shortcuts::Manager::get().matches(QStringLiteral("panels.rename"), k)) {
            if (selectedRows().size() == 1) renameDialog(selectedRows().first());
            return true;
        }
    }
    return DocPanel::eventFilter(o, e);
}

void StylesPanel::reload() {
    QString err;
    m_styles = styles::loadLibrary(&err);
    if (!err.isEmpty()) qWarning("styles: %s", qPrintable(err));
    fillList();
}

void StylesPanel::fillList() {
    QSet<QString> sel;
    for (QListWidgetItem *it : m_list->selectedItems()) sel.insert(it->data(Qt::UserRole).toString());
    const QString cur = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    m_filling = true;
    m_list->clear();
    for (int i = 0; i < m_styles.size(); ++i) {
        const Style &s = m_styles[i];
        auto *it = new QListWidgetItem(QIcon(QPixmap::fromImage(styles::preview(s, 44))), QString());
        it->setToolTip(QStringLiteral("<b>%1</b>%2<br>%3<br><i>Double-click to apply</i>")
                           .arg(s.name.toHtmlEscaped(), s.builtin ? QStringLiteral(" (built-in)") : QString(), styles::describe(s).toHtmlEscaped()));
        it->setData(Qt::UserRole, s.name);
        it->setData(Qt::UserRole + 1, i);   // position in m_styles: drag-reorder maps back through it
        it->setData(Qt::AccessibleTextRole, s.name);
        it->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsDragEnabled);
        m_list->addItem(it);
        if (sel.contains(s.name)) it->setSelected(true);
        if (s.name == cur) m_list->setCurrentItem(it, QItemSelectionModel::NoUpdate);
    }
    m_filling = false;
    rebuild();
}

void StylesPanel::syncOrderFromList() {
    if (m_filling || m_list->count() != m_styles.size()) return;
    QVector<Style> out;
    bool moved = false;
    for (int r = 0; r < m_list->count(); ++r) {
        const int i = m_list->item(r)->data(Qt::UserRole + 1).toInt();
        if (i < 0 || i >= m_styles.size()) return;
        moved |= i != r;
        out << m_styles[i];
    }
    if (!moved) return;
    m_styles = out;
    for (int r = 0; r < m_list->count(); ++r) m_list->item(r)->setData(Qt::UserRole + 1, r);
    saveUser();
    rebuild();
}

QList<int> StylesPanel::selectedRows() const {
    QList<int> r;
    for (QListWidgetItem *it : m_list->selectedItems()) r << m_list->row(it);
    std::sort(r.begin(), r.end());
    return r;
}

void StylesPanel::rebuild() {
    m_del->setEnabled(!selectedRows().isEmpty());
    m_new->setEnabled(true);   // with nothing selected the style takes the current colours
}

int StylesPanel::indexOf(const QString &name) const {
    for (int i = 0; i < m_styles.size(); ++i) if (m_styles[i].name == name) return i;
    return -1;
}

QString StylesPanel::uniqueName(const QString &base0, int except) const {
    QString base = base0.trimmed();
    if (base.isEmpty()) base = QStringLiteral("Style %1").arg(m_styles.size() + 1);
    auto taken = [&](const QString &n) { const int i = indexOf(n); return i >= 0 && i != except; };
    QString name = base;
    for (int n = 2; taken(name); ++n) name = QStringLiteral("%1 %2").arg(base).arg(n);
    return name;
}

bool StylesPanel::applyStyle(int index) {
    if (!m_doc || index < 0 || index >= m_styles.size()) return false;
    bool any = false;
    for (VObject *o : m_doc->selectedEditable()) any |= m_styles[index].applyTo(*o);
    if (any) m_doc->commit(QStringLiteral("Apply Style"));
    return any;
}

bool StylesPanel::saveUser() {
    QString err;
    if (!styles::saveLibrary(m_styles, &err)) {
        qWarning("styles: %s", qPrintable(err));
        return false;
    }
    return true;
}

bool StylesPanel::addStyle(const QString &name0, bool fill, bool stroke, bool effects, bool text) {
    const auto sel = m_doc ? m_doc->selectedEditable() : QVector<VObject *>();
    VObject src;
    if (!sel.isEmpty()) src = *sel.first();
    else {   // no selection: the current fill and stroke colours
        src.type = ObjType::Rect;
        src.fillOn = appState().fillOn;
        src.fill = appState().fill;
        src.strokeOn = appState().strokeOn;
        src.stroke = appState().stroke;
        src.strokeWidth = appState().strokeWidth;
        src.strokeKind = appState().strokeKind;
    }
    Style s = Style::fromObject(src, uniqueName(name0));
    s.hasFill = fill;
    s.hasStroke = stroke;
    s.hasEffects = effects;
    s.hasText = text && src.type == ObjType::Text;
    m_styles << s;
    fillList();
    m_list->clearSelection();
    m_list->item(m_styles.size() - 1)->setSelected(true);
    m_list->setCurrentRow(m_styles.size() - 1, QItemSelectionModel::NoUpdate);
    return saveUser();
}

int StylesPanel::deleteStyles(QList<int> rows) {
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    int n = 0;
    for (int i = rows.size() - 1; i >= 0; --i)
        if (rows[i] >= 0 && rows[i] < m_styles.size()) { m_styles.remove(rows[i]); ++n; }
    if (!n) return 0;
    m_list->clearSelection();
    fillList();
    saveUser();
    return n;
}

void StylesPanel::deleteSelectedAsk() {
    const QList<int> rows = selectedRows();
    if (rows.isEmpty()) return;
    if (!qApp->property("lw_testing").toBool()) {
        const QString what = rows.size() == 1 ? QStringLiteral("the style \u201c%1\u201d").arg(m_styles[rows.first()].name)
                                              : QStringLiteral("%1 styles").arg(rows.size());
        if (QMessageBox::question(this, QStringLiteral("Delete Styles"), QStringLiteral("Delete %1? (Reset Styles restores built-ins.)").arg(what)) != QMessageBox::Yes)
            return;
    }
    const int first = rows.first();
    deleteStyles(rows);
    if (!m_styles.isEmpty()) {   // keep a selection so Delete can be pressed again
        const int r = qMin(first, int(m_styles.size()) - 1);
        m_list->item(r)->setSelected(true);
        m_list->setCurrentRow(r, QItemSelectionModel::NoUpdate);
    }
}

bool StylesPanel::renameStyle(int index, const QString &name) {
    if (index < 0 || index >= m_styles.size()) return false;
    const QString n = name.trimmed();
    if (n.isEmpty() || (indexOf(n) >= 0 && indexOf(n) != index)) return false;
    m_styles[index].name = n;
    fillList();
    return saveUser();
}

void StylesPanel::renameDialog(int index) {
    if (index < 0 || index >= m_styles.size()) return;
    bool ok = false;
    const QString n = QInputDialog::getText(this, QStringLiteral("Rename Style"), QStringLiteral("Name:"), QLineEdit::Normal, m_styles[index].name, &ok);
    if (ok && !renameStyle(index, n) && n.trimmed() != m_styles[index].name)
        QMessageBox::warning(this, QStringLiteral("Rename Style"), QStringLiteral("A style called \u201c%1\u201d already exists.").arg(n.trimmed()));
}

bool StylesPanel::moveStyle(int from, int to) {
    if (from < 0 || from >= m_styles.size() || to < 0 || to >= m_styles.size() || from == to) return false;
    m_styles.move(from, to);
    m_list->clearSelection();
    fillList();
    m_list->item(to)->setSelected(true);
    m_list->setCurrentRow(to, QItemSelectionModel::NoUpdate);
    return saveUser();
}

bool StylesPanel::duplicateStyle(int index) {
    if (index < 0 || index >= m_styles.size()) return false;
    Style s = m_styles[index];
    s.builtin = false;
    s.name = uniqueName(s.name + QStringLiteral(" copy"));
    m_styles.insert(index + 1, s);
    m_list->clearSelection();
    fillList();
    m_list->item(index + 1)->setSelected(true);
    return saveUser();
}

bool StylesPanel::resetStyles() {
    QVector<Style> out = styles::builtins();
    for (const Style &s : m_styles) if (!s.builtin) out << s;
    // a user style that took a built-in's name keeps it with a suffix
    for (int i = styles::builtins().size(); i < out.size(); ++i)
        for (int n = 2; std::any_of(out.begin(), out.begin() + i, [&](const Style &o) { return o.name == out[i].name; }); ++n)
            out[i].name = QStringLiteral("%1 %2").arg(out[i].name).arg(n);
    m_styles = out;
    m_list->clearSelection();
    fillList();
    return saveUser();
}

int StylesPanel::importStyles(const QString &path, QString *err) {
    const QVector<Style> in = styles::importFile(path, err);
    for (Style s : in) {
        s.name = uniqueName(s.name);
        m_styles << s;
    }
    if (in.isEmpty()) return 0;
    m_list->clearSelection();
    fillList();
    for (int i = m_styles.size() - in.size(); i < m_styles.size(); ++i) m_list->item(i)->setSelected(true);
    saveUser();
    return in.size();
}

bool StylesPanel::exportStyles(const QString &path, QList<int> rows, QString *err) {
    QVector<Style> list;
    if (rows.isEmpty()) list = m_styles;
    else for (int r : rows) if (r >= 0 && r < m_styles.size()) list << m_styles[r];
    return styles::exportFile(path, list, err);
}

void StylesPanel::newStyleDialog() {
    const VObject *o = m_doc && !m_doc->selectedEditable().isEmpty() ? m_doc->selectedEditable().first() : nullptr;
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("newStyleDialog"));
    dlg.setWindowTitle(QStringLiteral("New Style"));
    auto *f = new QFormLayout(&dlg);
    auto *name = new QLineEdit(uniqueName(QStringLiteral("Style %1").arg(m_styles.size() + 1)));
    name->setObjectName(QStringLiteral("styleName"));
    f->addRow(QStringLiteral("Name:"), name);
    if (!o) {
        auto *l = new QLabel(QStringLiteral("Nothing is selected: the style takes the current fill and stroke colours."));
        l->setObjectName(QStringLiteral("styleFromColours"));
        l->setWordWrap(true);
        f->addRow(l);
    }
    auto box = [&](const char *obj, const QString &label, bool on) {
        auto *c = new QCheckBox(label);
        c->setObjectName(QString::fromLatin1(obj));
        c->setChecked(on);
        f->addRow(QString(), c);
        return c;
    };
    const bool vec = !o || o->isVector();
    QCheckBox *fill = box("styleFill", QStringLiteral("Fill type and colour"), vec);
    QCheckBox *stroke = box("styleStroke", QStringLiteral("Stroke type and colour"), vec);
    QCheckBox *fx = box("styleEffects", QStringLiteral("Effect"), true);
    QCheckBox *text = box("styleText", QStringLiteral("Text font, size and style"), o && o->type == ObjType::Text);
    text->setEnabled(o && o->type == ObjType::Text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    f->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    if (!addStyle(name->text(), fill->isChecked(), stroke->isChecked(), fx->isChecked(), text->isChecked()))
        QMessageBox::warning(this, QStringLiteral("New Style"), QStringLiteral("Could not save the style to %1.").arg(styles::userFile()));
}

// ---------------------------------------------------------------- StubPanel
StubPanel::StubPanel(const QString &what, QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    auto *l = new QLabel(QStringLiteral("%1 is not available yet.").arg(what));
    l->setWordWrap(true);
    l->setObjectName(QStringLiteral("StubNote"));   // themed (#StubNote)
    v->addWidget(l);
}

// ---------------------------------------------------------------- Library
bool symbolPropertiesDialog(QWidget *parent, const QString &title, QString &name, SymbolKind &kind) {
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("symbolDialog"));
    dlg.setWindowTitle(title);
    auto *f = new QFormLayout(&dlg);
    auto *ed = new QLineEdit(name);
    ed->setObjectName(QStringLiteral("symbolName"));
    f->addRow(QStringLiteral("Name:"), ed);
    auto *grp = new QButtonGroup(&dlg);
    auto *box = new QVBoxLayout;
    const char *ids[] = {"kindGraphic", "kindAnimation", "kindButton"};
    for (int i = 0; i < 3; ++i) {
        auto *rb = new QRadioButton(symbolKindLabel(SymbolKind(i)));
        rb->setObjectName(QString::fromLatin1(ids[i]));
        rb->setChecked(int(kind) == i);
        if (SymbolKind(i) == SymbolKind::Button && !webFeaturesEnabled() && kind != SymbolKind::Button)
            rb->setVisible(false);
        grp->addButton(rb, i);
        box->addWidget(rb);
    }
    f->addRow(QStringLiteral("Type:"), box);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    f->addRow(bb);
    ed->selectAll();
    if (dlg.exec() != QDialog::Accepted) return false;
    name = ed->text().trimmed();
    if (name.isEmpty()) name = QStringLiteral("Symbol");
    kind = SymbolKind(qMax(0, grp->checkedId()));
    return true;
}

namespace {
const char *kSymbolMime = "application/x-libreworks-symbol";
class SymbolList : public QListWidget {
public:
    using QListWidget::QListWidget;
    QPointer<Document> doc;
protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(kSymbolMime)}; }
    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override {
        if (items.isEmpty()) return nullptr;
        auto *m = new QMimeData;
        m->setData(QString::fromLatin1(kSymbolMime), QByteArray::number(quintptr(doc.data())) + ':' +
                                                         QByteArray::number(items.first()->data(Qt::UserRole).toULongLong()));
        return m;
    }
};
}

QIcon LibraryPanel::preview(const Document &doc, const Symbol &sym, int size) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    SymbolScope scope(&doc.s.symbols);
    QRectF b = sym.visualBounds();
    if (!b.isEmpty() && !sym.frames.isEmpty()) {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const double sc = qMin((size - 4) / b.width(), (size - 4) / b.height());
        p.translate(size / 2.0, size / 2.0);
        p.scale(sc, sc);
        p.translate(-b.center());
        for (const VObject &o : sym.frames.first()) if (o.visible) render::drawObject(p, o, QRect(-1000000, -1000000, 2000000, 2000000));
    }
    {
        QPainter p(&img);
        p.setPen(QColor(0x99, 0x99, 0x99));
        p.drawRect(0, 0, size - 1, size - 1);
        // kind badge
        const QString k = sym.kind == SymbolKind::Button ? QStringLiteral("B") : sym.kind == SymbolKind::Animation ? QStringLiteral("A") : QStringLiteral("G");
        QRect br(size - 11, size - 11, 10, 10);
        p.fillRect(br, sym.kind == SymbolKind::Button ? QColor(0x2a, 0x7a, 0xd0) : sym.kind == SymbolKind::Animation ? QColor(0xd0, 0x7a, 0x2a) : QColor(0x55, 0x99, 0x44));
        QFont f = p.font(); f.setPixelSize(8); f.setBold(true); p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(br, Qt::AlignCenter, k);
    }
    return QIcon(QPixmap::fromImage(img));
}

LibraryPanel::LibraryPanel(QWidget *parent) : DocPanel(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_count = new QLabel;
    m_count->setObjectName(QStringLiteral("libraryCount"));
    m_count->setContentsMargins(4, 2, 4, 2);
    v->addWidget(m_count);
    auto *list = new SymbolList;
    m_list = list;
    m_list->setObjectName(QStringLiteral("libraryList"));
    m_list->setIconSize(QSize(32, 32));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setDragEnabled(true);
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setMinimumHeight(48);
    m_list->setMaximumHeight(QWIDGETSIZE_MAX);
    m_list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_list, 1);
    QHBoxLayout *bar = buttonBar(v);
    auto mk = [&](QToolButton *&b, const QIcon &ic, const QString &tip, const char *name) {
        b = new QToolButton; b->setIcon(ic); b->setToolTip(tip); b->setAutoRaise(true); b->setFixedSize(24, 22);
        b->setObjectName(QString::fromLatin1(name));
        bar->addWidget(b);
    };
    mk(m_propB, icons::named(QStringLiteral("default")), QStringLiteral("Symbol Properties..."), "libProps");
    mk(m_editB, icons::tool(Tool::Subselect), QStringLiteral("Edit Symbol"), "libEdit");
    mk(m_insB, icons::named(QStringLiteral("down")), QStringLiteral("Insert an instance"), "libInsert");
    bar->addStretch();
    mk(m_newB, icons::named(QStringLiteral("plus")), QStringLiteral("New Symbol..."), "libNew");
    mk(m_dupB, icons::named(QStringLiteral("dup")), QStringLiteral("Duplicate Symbol"), "libDup");
    mk(m_delB, icons::named(QStringLiteral("trash")), QStringLiteral("Delete Symbol"), "libDelete");
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) { emit editSymbol(it->data(Qt::UserRole).toULongLong()); });
    connect(m_list, &QListWidget::currentRowChanged, this, [this] {
        const bool on = currentSymbol() != 0;
        for (QToolButton *b : {m_dupB, m_editB, m_insB, m_delB, m_propB}) b->setEnabled(on);
    });
    connect(m_newB, &QToolButton::clicked, this, [this] { newSymbol(); });
    connect(m_dupB, &QToolButton::clicked, this, [this] { duplicateCurrent(); });
    connect(m_editB, &QToolButton::clicked, this, [this] { if (quint64 id = currentSymbol()) emit editSymbol(id); });
    connect(m_insB, &QToolButton::clicked, this, [this] { insertCurrent(); });
    connect(m_delB, &QToolButton::clicked, this, [this] { deleteCurrent(); });
    connect(m_propB, &QToolButton::clicked, this, [this] { propertiesCurrent(); });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu;
        const bool on = m_list->itemAt(pos) != nullptr;
        if (on) m_list->setCurrentItem(m_list->itemAt(pos));
        menu.addAction(QStringLiteral("New Symbol..."), this, [this] { newSymbol(); })->setEnabled(m_doc);
        menu.addAction(QStringLiteral("Edit Symbol"), this, [this] { if (quint64 id = currentSymbol()) emit editSymbol(id); })->setEnabled(on);
        menu.addAction(QStringLiteral("Insert Instance"), this, [this] { insertCurrent(); })->setEnabled(on);
        menu.addAction(QStringLiteral("Duplicate"), this, [this] { duplicateCurrent(); })->setEnabled(on);
        menu.addAction(QStringLiteral("Properties..."), this, [this] { propertiesCurrent(); })->setEnabled(on);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete"), this, [this] { deleteCurrent(); })->setEnabled(on);
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    rebuild();
}

quint64 LibraryPanel::currentSymbol() const {
    QListWidgetItem *it = m_list->currentItem();
    return it ? it->data(Qt::UserRole).toULongLong() : 0;
}

void LibraryPanel::setCurrentSymbol(quint64 id) {
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toULongLong() == id) { m_list->setCurrentRow(i); return; }
}

void LibraryPanel::rebuild() {
    static_cast<SymbolList *>(m_list)->doc = m_doc;
    const quint64 keep = currentSymbol();
    QSignalBlocker block(m_list);
    m_list->clear();
    if (m_doc) {
        for (const Symbol &sym : m_doc->s.symbols) {
            auto *it = new QListWidgetItem(preview(*m_doc, sym), sym.name, m_list);
            it->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(sym.id));
            it->setToolTip(QStringLiteral("%1 symbol, %2 frame(s), %3 instance(s)").arg(symbolKindLabel(sym.kind)).arg(sym.frameCount()).arg(m_doc->instanceCount(sym.id)));
        }
    }
    m_count->setText(m_doc ? QStringLiteral("%1 item(s) in library").arg(m_doc->s.symbols.size()) : QString());
    setCurrentSymbol(keep);
    if (!m_list->currentItem() && m_list->count()) m_list->setCurrentRow(0);
    block.unblock();
    const bool on = currentSymbol() != 0;
    for (QToolButton *b : {m_dupB, m_editB, m_insB, m_delB, m_propB}) b->setEnabled(on);
    m_newB->setEnabled(m_doc);
}

bool LibraryPanel::insertCurrent() {
    const quint64 id = currentSymbol();
    if (!m_doc || !id) return false;
    if (m_doc->insertInstance(id, QPointF(m_doc->s.width / 2.0, m_doc->s.height / 2.0)).isEmpty()) return false;
    m_doc->commit(QStringLiteral("Insert Symbol"));
    return true;
}

bool LibraryPanel::deleteCurrent(bool ask) {
    const quint64 id = currentSymbol();
    if (!m_doc || !id) return false;
    const int n = m_doc->instanceCount(id);
    if (ask && n > 0 && !qApp->property("lw_testing").toBool() &&
        QMessageBox::question(this, QStringLiteral("Delete Symbol"), QStringLiteral("The symbol has %1 instance(s) in the document. Delete the symbol and its instances?").arg(n)) != QMessageBox::Yes)
        return false;
    if (!m_doc->deleteSymbol(id)) return false;
    m_doc->commit(QStringLiteral("Delete Symbol"));
    return true;
}

quint64 LibraryPanel::duplicateCurrent() {
    const quint64 id = currentSymbol();
    if (!m_doc || !id) return 0;
    const quint64 n = m_doc->duplicateSymbol(id);
    if (n) { m_doc->commit(QStringLiteral("Duplicate Symbol")); rebuild(); setCurrentSymbol(n); }
    return n;
}

bool LibraryPanel::propertiesCurrent() {
    Symbol *sym = m_doc ? m_doc->symbol(currentSymbol()) : nullptr;
    if (!sym) return false;
    QString name = sym->name;
    SymbolKind kind = sym->kind;
    const quint64 id = sym->id;
    if (!symbolPropertiesDialog(this, QStringLiteral("Symbol Properties"), name, kind)) return false;
    sym = m_doc->symbol(id);
    if (!sym || (name == sym->name && kind == sym->kind)) return false;
    if (name != sym->name) sym->name = m_doc->uniqueSymbolName(name);
    if (kind != sym->kind) {
        sym->kind = kind;
        m_doc->setSymbolFrames(id, sym->frames); // pads / truncates frames for the new kind
    }
    m_doc->commit(QStringLiteral("Symbol Properties"));
    return true;
}

quint64 LibraryPanel::newSymbol() {
    if (!m_doc) return 0;
    QString name = m_doc->uniqueSymbolName(QStringLiteral("Symbol"));
    SymbolKind kind = SymbolKind::Graphic;
    if (!symbolPropertiesDialog(this, QStringLiteral("New Symbol"), name, kind)) return 0;
    Symbol sym;
    sym.name = name;
    sym.kind = kind;
    const quint64 id = m_doc->addSymbol(sym);
    m_doc->commit(QStringLiteral("New Symbol"));
    rebuild();
    setCurrentSymbol(id);
    emit editSymbol(id);
    return id;
}

AutoShapesPanel::AutoShapesPanel(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("AutoShapesPanel"));
    auto *g = new QGridLayout(this);
    g->setContentsMargins(4, 4, 4, 4);
    g->setSpacing(2);
    const QStringList kinds = autoshape::kinds();
    for (int i = 0; i < kinds.size(); ++i) {
        const QString k = kinds[i];
        const Tool t = autoShapeTool(k);
        auto *b = new QToolButton;
        b->setObjectName(QStringLiteral("autoshape_") + k);
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setIcon(icons::tool(t));
        b->setIconSize(QSize(24, 24));
        b->setFixedSize(34, 34);
        b->setToolTip(QStringLiteral("<b>%1</b><br>%2").arg(autoshape::label(k), autoshape::tip(k)));
        connect(b, &QToolButton::clicked, this, [t] { appState().setTool(t); });
        g->addWidget(b, i / 6, i % 6);
        m_buttons << b;
    }
    g->setColumnStretch(6, 1);
    connect(&appState(), &AppState::toolChanged, this, [this](Tool) { sync(); });
    sync();
}

void AutoShapesPanel::sync() {
    const QString cur = toolAutoShapeKind(appState().tool());
    for (QToolButton *b : m_buttons) b->setChecked(b->objectName() == QStringLiteral("autoshape_") + cur);
}
