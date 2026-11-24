#pragma once
// Window > Find & Replace (v0.3.1), as in Fireworks: find and replace text, fonts, colours, URLs and
// non-web-safe colours in the document, the selection, every frame or every open document.
#include "core/FindReplace.h"
#include <QPointer>
#include <QWidget>
#include <functional>

class Document;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QStackedWidget;
class QListWidget;
class QLabel;
class QPushButton;
class ColorButton;

class FindPanel : public QWidget {
    Q_OBJECT
public:
    explicit FindPanel(QWidget *parent = nullptr);
    // The open documents in tab order, the current one, and how to bring one to the front.
    void setDocumentProvider(std::function<QVector<Document *>()> all, std::function<Document *()> current,
                             std::function<void(Document *)> activate);

    fr::Query query() const;
    void setQuery(const fr::Query &q);
    fr::Scope scope() const;

    // The buttons; each returns what it did (false = nothing found / replaced).
    bool findNext();
    bool replaceCurrent();      // replaces the current match, then finds the next (with no current match it only finds)
    int replaceAll();           // one undo step per document that changed
    QString statusText() const;
    QVector<fr::Hit> lastHits() const { return m_hits; }

signals:
    void searched();

private:
    QVector<Document *> scopeDocs() const;
    void queryEdited();
    void updatePage();
    void refreshWhat();
    void refreshList();
    void select(Document *d, const fr::Hit &h);
    void setStatus(const QString &s);
    const QSet<quint64> *scopeSelection(Document *d);

    std::function<QVector<Document *>()> m_all;
    std::function<Document *()> m_current;
    std::function<void(Document *)> m_activate;

    QComboBox *m_scope, *m_what;
    QStackedWidget *m_pages;
    QLineEdit *m_findText, *m_replaceText, *m_findUrl, *m_replaceUrl;
    QCheckBox *m_whole, *m_case, *m_regex;
    QWidget *m_textOpts, *m_colorOpts;
    QComboBox *m_fontFrom, *m_fontTo;
    QDoubleSpinBox *m_sizeFrom, *m_sizeTo;
    ColorButton *m_colorFrom, *m_colorTo;
    QSpinBox *m_tolerance;
    QCheckBox *m_fills, *m_strokes, *m_gradients, *m_effects, *m_pixels;
    QPushButton *m_findBtn, *m_replaceBtn, *m_replaceAllBtn;
    QListWidget *m_list;
    QLabel *m_status;

    // the current match: a document, a frame and an object
    QPointer<Document> m_curDoc;
    fr::Hit m_cur;
    bool m_haveCur = false;
    // Selection scope: what was selected when the search started
    QPointer<Document> m_selDoc;
    QSet<quint64> m_sel;
    quint64 m_lastSelected = 0;   // the object Find last selected
    QVector<fr::Hit> m_hits;
    bool m_loading = false;
};
