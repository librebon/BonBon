#include <QListView>
#include "PopupSlider.h"
#include "FindPanel.h"
#include "AppState.h"
#include "Dialogs.h"
#include "core/Document.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include "WideFontCombo.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <tuple>

FindPanel::FindPanel(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);
    m_scope = new QComboBox;
    m_scope->setObjectName(QStringLiteral("findScope"));
    m_scope->addItems(fr::scopeNames());
    m_what = new QComboBox;
    m_what->setObjectName(QStringLiteral("findWhat"));
    v->addWidget(m_scope);
    refreshWhat();
    v->addWidget(m_what);

    m_pages = new QStackedWidget;
    auto form = [](QWidget *w) { auto *f = new QFormLayout(w); f->setContentsMargins(0, 0, 0, 0); f->setSpacing(4); return f; };
    // text
    auto *pt = new QWidget; auto *ft = form(pt);
    m_findText = new QLineEdit; m_findText->setObjectName(QStringLiteral("findText"));
    m_replaceText = new QLineEdit; m_replaceText->setObjectName(QStringLiteral("replaceText"));
    ft->addRow(QStringLiteral("Find:"), m_findText);
    ft->addRow(QStringLiteral("Change to:"), m_replaceText);
    m_pages->addWidget(pt);
    // font
    auto *pf = new QWidget; auto *ff = form(pf);
    auto fontBox = [&](const QString &name, const QString &placeholder) {
        auto *c = new WidePopupComboBox; c->setObjectName(name); c->setEditable(true);
        fillFontSourceCombo(c, true);
        // v0.3.2: ~1000 families; without uniform item sizes every theme / style change re-measured
        // (text-shaped) every row of both hidden pop-up lists, doubling the theme-switch time
        if (auto *lv = qobject_cast<QListView *>(c->view())) lv->setUniformItemSizes(true);
        c->lineEdit()->setPlaceholderText(placeholder);
        c->setMinimumContentsLength(10);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        return c;
    };
    auto sizeBox = [](const QString &name, const QString &special) {
        auto *s = new PopupSliderDoubleField; s->setObjectName(name); s->setRange(0, 2000); s->setDecimals(1);
        s->setSpecialValueText(special); s->setSuffix(QStringLiteral(" pt"));
        return s;
    };
    m_fontFrom = fontBox(QStringLiteral("findFont"), QStringLiteral("Any font"));
    m_sizeFrom = sizeBox(QStringLiteral("findSize"), QStringLiteral("Any size"));
    m_fontTo = fontBox(QStringLiteral("replaceFont"), QStringLiteral("Unchanged"));
    connect(&appState(), &AppState::prefsChanged, this, [this] {
        fillFontSourceCombo(m_fontFrom, true);
        fillFontSourceCombo(m_fontTo, true);
    });

    m_sizeTo = sizeBox(QStringLiteral("replaceSize"), QStringLiteral("Unchanged"));
    ff->addRow(QStringLiteral("Find:"), m_fontFrom);
    ff->addRow(QString(), m_sizeFrom);
    ff->addRow(QStringLiteral("Change to:"), m_fontTo);
    ff->addRow(QString(), m_sizeTo);
    m_pages->addWidget(pf);
    // colour
    auto *pc = new QWidget; auto *fc = form(pc);
    m_colorFrom = new ColorButton(nullptr, false); m_colorFrom->setObjectName(QStringLiteral("findColor")); m_colorFrom->setColor(Qt::white);
    m_colorTo = new ColorButton(nullptr, true); m_colorTo->setObjectName(QStringLiteral("replaceColor"));
    m_tolerance = new PopupSliderField; m_tolerance->setObjectName(QStringLiteral("findTolerance")); m_tolerance->setRange(0, 255);
    fc->addRow(QStringLiteral("Find:"), m_colorFrom);
    fc->addRow(QStringLiteral("Change to:"), m_colorTo);
    fc->addRow(QStringLiteral("Tolerance:"), m_tolerance);
    m_pages->addWidget(pc);
    // URL
    auto *pu = new QWidget; auto *fu = form(pu);
    m_findUrl = new QLineEdit; m_findUrl->setObjectName(QStringLiteral("findUrl")); m_findUrl->setPlaceholderText(QStringLiteral("Any URL"));
    m_replaceUrl = new QLineEdit; m_replaceUrl->setObjectName(QStringLiteral("replaceUrl"));
    fu->addRow(QStringLiteral("Find:"), m_findUrl);
    fu->addRow(QStringLiteral("Change to:"), m_replaceUrl);
    m_pages->addWidget(pu);
    // non-web-safe
    auto *pn = new QLabel(QStringLiteral("Finds colours outside the web-safe 216 and changes them to the nearest web-safe colour."));
    pn->setWordWrap(true);
    m_pages->addWidget(pn);
    v->addWidget(m_pages);

    m_textOpts = new QWidget;
    auto *to = new QHBoxLayout(m_textOpts); to->setContentsMargins(0, 0, 0, 0);
    m_whole = new QCheckBox(QStringLiteral("Whole word")); m_whole->setObjectName(QStringLiteral("findWholeWord"));
    m_case = new QCheckBox(QStringLiteral("Match case")); m_case->setObjectName(QStringLiteral("findMatchCase"));
    m_regex = new QCheckBox(QStringLiteral("Regular expressions")); m_regex->setObjectName(QStringLiteral("findRegex"));
    auto *tov = new QVBoxLayout; tov->setSpacing(2);
    auto *row = new QHBoxLayout; row->addWidget(m_whole); row->addWidget(m_case); row->addStretch(1);
    tov->addLayout(row); tov->addWidget(m_regex);
    to->addLayout(tov);
    v->addWidget(m_textOpts);

    m_colorOpts = new QWidget;
    auto *co = new QVBoxLayout(m_colorOpts); co->setContentsMargins(0, 0, 0, 0); co->setSpacing(2);
    auto chk = [](const QString &t, const QString &n, bool on) { auto *c = new QCheckBox(t); c->setObjectName(n); c->setChecked(on); return c; };
    m_fills = chk(QStringLiteral("Fills"), QStringLiteral("findFills"), true);
    m_strokes = chk(QStringLiteral("Strokes"), QStringLiteral("findStrokes"), true);
    m_gradients = chk(QStringLiteral("Gradients"), QStringLiteral("findGradients"), true);
    m_effects = chk(QStringLiteral("Effects"), QStringLiteral("findEffects"), true);
    m_pixels = chk(QStringLiteral("Bitmap pixels"), QStringLiteral("findPixels"), false);
    auto *r1 = new QHBoxLayout; r1->addWidget(m_fills); r1->addWidget(m_strokes); r1->addWidget(m_gradients); r1->addStretch(1);
    auto *r2 = new QHBoxLayout; r2->addWidget(m_effects); r2->addWidget(m_pixels); r2->addStretch(1);
    co->addLayout(r1); co->addLayout(r2);
    v->addWidget(m_colorOpts);

    auto *btns = new QHBoxLayout;
    m_findBtn = new QPushButton(QStringLiteral("Find")); m_findBtn->setObjectName(QStringLiteral("findNext"));
    m_replaceBtn = new QPushButton(QStringLiteral("Replace")); m_replaceBtn->setObjectName(QStringLiteral("findReplace"));
    m_replaceAllBtn = new QPushButton(QStringLiteral("Replace All")); m_replaceAllBtn->setObjectName(QStringLiteral("findReplaceAll"));
    btns->addWidget(m_findBtn); btns->addWidget(m_replaceBtn); btns->addWidget(m_replaceAllBtn);
    v->addLayout(btns);
    m_list = new QListWidget; m_list->setObjectName(QStringLiteral("findResults"));
    m_list->setMinimumHeight(80);
    v->addWidget(m_list, 1);
    m_status = new QLabel; m_status->setObjectName(QStringLiteral("findStatus")); m_status->setWordWrap(true);
    v->addWidget(m_status);

    connect(m_what, &QComboBox::currentIndexChanged, this, [this] { updatePage(); queryEdited(); });
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this] { queryEdited(); });
    for (QLineEdit *e : {m_findText, m_replaceText, m_findUrl, m_replaceUrl}) connect(e, &QLineEdit::textEdited, this, [this] { queryEdited(); });
    connect(m_findText, &QLineEdit::returnPressed, this, [this] { findNext(); });
    connect(m_findUrl, &QLineEdit::returnPressed, this, [this] { findNext(); });
    for (QCheckBox *c : {m_whole, m_case, m_regex, m_fills, m_strokes, m_gradients, m_effects, m_pixels})
        connect(c, &QCheckBox::toggled, this, [this] { queryEdited(); });
    for (QComboBox *c : {m_fontFrom, m_fontTo}) connect(c, &QComboBox::currentTextChanged, this, [this] { queryEdited(); });
    for (QDoubleSpinBox *s : {m_sizeFrom, m_sizeTo}) connect(s, &QDoubleSpinBox::valueChanged, this, [this] { queryEdited(); });
    connect(m_tolerance, &QSpinBox::valueChanged, this, [this] { queryEdited(); });
    connect(m_colorFrom, &ColorButton::colorPicked, this, [this] { queryEdited(); });
    connect(m_colorTo, &ColorButton::colorPicked, this, [this] { queryEdited(); });
    connect(m_findBtn, &QPushButton::clicked, this, [this] { findNext(); });
    connect(m_replaceBtn, &QPushButton::clicked, this, [this] { replaceCurrent(); });
    connect(m_replaceAllBtn, &QPushButton::clicked, this, [this] { replaceAll(); });
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        const int i = m_list->row(it);
        if (i < 0 || i >= m_hits.size()) return;
        Document *d = static_cast<Document *>(it->data(Qt::UserRole).value<void *>());
        if (!m_all || !m_all().contains(d)) return;
        select(d, m_hits[i]);
    });
    connect(&appState(), &AppState::prefsChanged, this, [this] { refreshWhat(); updatePage(); queryEdited(); });
    updatePage();
}

void FindPanel::setDocumentProvider(std::function<QVector<Document *>()> all, std::function<Document *()> current,
                                    std::function<void(Document *)> activate) {
    m_all = std::move(all);
    m_current = std::move(current);
    m_activate = std::move(activate);
}

fr::Query FindPanel::query() const {
    fr::Query q;
    q.what = fr::What(m_what->currentData().toInt());
    if (q.what == fr::What::Url) { q.find = m_findUrl->text(); q.replace = m_replaceUrl->text(); }
    else { q.find = m_findText->text(); q.replace = m_replaceText->text(); }
    q.caseSensitive = m_case->isChecked();
    q.wholeWord = m_whole->isChecked();
    q.regex = m_regex->isChecked();
    q.fontFrom = m_fontFrom->currentText().trimmed();
    q.fontTo = m_fontTo->currentText().trimmed();
    q.sizeFrom = m_sizeFrom->value();
    q.sizeTo = m_sizeTo->value();
    q.from = m_colorFrom->color();
    q.to = m_colorTo->color();
    q.tolerance = m_tolerance->value();
    q.fills = m_fills->isChecked();
    q.strokes = m_strokes->isChecked();
    q.gradients = m_gradients->isChecked();
    q.effects = m_effects->isChecked();
    q.pixels = m_pixels->isChecked();
    return q;
}

void FindPanel::setQuery(const fr::Query &q) {
    m_loading = true;
    {
        const int i = m_what->findData(int(q.what));
        m_what->setCurrentIndex(i >= 0 ? i : 0);
    }
    if (q.what == fr::What::Url) { m_findUrl->setText(q.find); m_replaceUrl->setText(q.replace); }
    else { m_findText->setText(q.find); m_replaceText->setText(q.replace); }
    m_case->setChecked(q.caseSensitive);
    m_whole->setChecked(q.wholeWord);
    m_regex->setChecked(q.regex);
    m_fontFrom->setEditText(q.fontFrom);
    m_fontTo->setEditText(q.fontTo);
    m_sizeFrom->setValue(q.sizeFrom);
    m_sizeTo->setValue(q.sizeTo);
    m_colorFrom->setColor(q.from);
    m_colorTo->setColor(q.to);
    m_tolerance->setValue(q.tolerance);
    m_fills->setChecked(q.fills);
    m_strokes->setChecked(q.strokes);
    m_gradients->setChecked(q.gradients);
    m_effects->setChecked(q.effects);
    m_pixels->setChecked(q.pixels);
    m_loading = false;
    updatePage();
    queryEdited();
}

fr::Scope FindPanel::scope() const { return fr::Scope(m_scope->currentIndex()); }

void FindPanel::refreshWhat() {
    const fr::What cur = m_what->count() ? fr::What(m_what->currentData().toInt()) : fr::What::Text;
    QSignalBlocker b(m_what);
    m_what->clear();
    const QStringList names = fr::whatNames();
    for (int i = 0; i < names.size(); ++i) {
        if (fr::What(i) == fr::What::Url && !webFeaturesEnabled()) continue;
        m_what->addItem(names[i], i);
    }
    const int want = (cur == fr::What::Url && !webFeaturesEnabled()) ? int(fr::What::Text) : int(cur);
    int idx = m_what->findData(want);
    if (idx < 0) idx = 0;
    m_what->setCurrentIndex(idx);
}

void FindPanel::updatePage() {
    const fr::What w = fr::What(m_what->currentData().toInt());
    m_pages->setCurrentIndex(int(w));
    m_textOpts->setVisible(w == fr::What::Text || w == fr::What::Url);
    m_colorOpts->setVisible(w == fr::What::Color || w == fr::What::NonWebColor);
    m_replaceBtn->setText(QStringLiteral("Replace"));
}

// A new query starts a new search.
void FindPanel::queryEdited() {
    if (m_loading) return;
    m_haveCur = false;
    m_curDoc = nullptr;
    m_hits.clear();
    m_list->clear();
    m_status->clear();
}

void FindPanel::setStatus(const QString &s) { m_status->setText(s); }
QString FindPanel::statusText() const { return m_status->text(); }

QVector<Document *> FindPanel::scopeDocs() const {
    QVector<Document *> out;
    Document *cur = m_current ? m_current() : nullptr;
    if (scope() != fr::Scope::AllDocuments) {
        if (cur) out << cur;
        return out;
    }
    return m_all ? m_all() : QVector<Document *>();   // tab order
}

const QSet<quint64> *FindPanel::scopeSelection(Document *d) {
    if (scope() != fr::Scope::Selection || !d) return nullptr;
    // the selection the search started from; selecting a match does not narrow it
    const bool ownMatch = m_lastSelected && d->selection == QSet<quint64>{m_lastSelected};
    if (m_selDoc != d || !ownMatch) { m_selDoc = d; m_sel = d->selection; }
    return &m_sel;
}

void FindPanel::select(Document *d, const fr::Hit &h) {
    if (m_activate && m_current && m_current() != d) m_activate(d);
    if (d->curFrame != h.frame) d->setCurrentFrame(h.frame);
    m_curDoc = d;
    m_cur = h;
    m_haveCur = true;
    m_lastSelected = h.objectId;
    d->selectOnly(h.objectId);
}

void FindPanel::refreshList() {
    m_list->clear();
    m_hits.clear();
    const fr::Query q = query();
    for (Document *d : scopeDocs()) {
        const QVector<fr::Hit> hs = fr::findAll(*d, q, scope(), scopeSelection(d));
        for (const fr::Hit &h : hs) {
            const VObject &o = d->s.frames[h.frame].layers[h.layer][h.index];
            QString t = QStringLiteral("%1 \u2013 %2").arg(o.name.isEmpty() ? QStringLiteral("Object") : o.name, h.detail);
            if (d->s.frames.size() > 1) t = QStringLiteral("Frame %1: ").arg(h.frame + 1) + t;
            if (scope() == fr::Scope::AllDocuments) t = d->title() + QStringLiteral(": ") + t;
            auto *it = new QListWidgetItem(t, m_list);
            it->setData(Qt::UserRole, QVariant::fromValue<void *>(d));
            m_hits << h;
        }
    }
}

bool FindPanel::findNext() {
    const QVector<Document *> docs = scopeDocs();
    if (docs.isEmpty()) { setStatus(QStringLiteral("No document is open.")); return false; }
    const fr::Query q = query();
    if ((q.what == fr::What::Text) && q.find.isEmpty()) { setStatus(QStringLiteral("Type the text to find.")); return false; }
    refreshList();
    if (m_hits.isEmpty()) {
        m_haveCur = false;
        setStatus(QStringLiteral("Not found."));
        emit searched();
        return false;
    }
    // the first match after the current one, in document / frame / layer / stacking order
    auto docOf = [&](int row) { return static_cast<Document *>(m_list->item(row)->data(Qt::UserRole).value<void *>()); };
    int next = 0;
    bool wrapped = false;
    if (m_haveCur && m_curDoc) {
        const int cd = docs.indexOf(m_curDoc.data());
        const auto curKey = std::make_tuple(cd, m_cur.frame, m_cur.layer, m_cur.index);
        next = -1;
        for (int r = 0; r < m_hits.size(); ++r) {
            const fr::Hit &h = m_hits[r];
            if (std::make_tuple(int(docs.indexOf(docOf(r))), h.frame, h.layer, h.index) > curKey) { next = r; break; }
        }
        if (next < 0) { next = 0; wrapped = true; }
    } else {
        // a new search starts in the current document
        const int cd = docs.indexOf(m_current ? m_current() : nullptr);
        for (int r = 0; r < m_hits.size(); ++r)
            if (docs.indexOf(docOf(r)) >= cd) { next = r; break; }
    }
    m_list->setCurrentRow(next);
    select(docOf(next), m_hits[next]);
    setStatus(QStringLiteral("%1 of %2 matches%3").arg(next + 1).arg(m_hits.size()).arg(wrapped ? QStringLiteral(" (wrapped)") : QString()));
    emit searched();
    return true;
}

bool FindPanel::replaceCurrent() {
    const fr::Query q = query();
    Document *d = m_curDoc.data();
    const bool valid = m_haveCur && d && m_all && m_all().contains(d) && d->find(m_cur.objectId);
    if (!valid) { findNext(); return false; }
    int n = 0;
    if (d->s.frames.size() > m_cur.frame) {
        // the match may have moved (another edit): replace in the object wherever it now is
        n = fr::replaceHit(*d, q, m_cur);
    }
    if (n) d->commit(QStringLiteral("Replace"));
    // keep the position: the next search continues after this object
    findNext();
    if (n) setStatus(QStringLiteral("Replaced. ") + statusText());
    return n > 0;
}

int FindPanel::replaceAll() {
    const QVector<Document *> docs = scopeDocs();
    if (docs.isEmpty()) { setStatus(QStringLiteral("No document is open.")); return 0; }
    const fr::Query q = query();
    if (q.what == fr::What::Text && q.find.isEmpty()) { setStatus(QStringLiteral("Type the text to find.")); return 0; }
    int total = 0, docsChanged = 0;
    for (Document *d : docs) {
        const int n = fr::replaceAll(*d, q, scope(), scopeSelection(d));
        if (n) { d->commit(QStringLiteral("Replace All")); total += n; ++docsChanged; }
    }
    m_haveCur = false;
    m_hits.clear();
    m_list->clear();
    setStatus(total ? (docsChanged > 1 ? QStringLiteral("%1 replacements in %2 documents.").arg(total).arg(docsChanged)
                                       : QStringLiteral("%1 replacements.").arg(total))
                    : QStringLiteral("Nothing to replace."));
    emit searched();
    return total;
}
