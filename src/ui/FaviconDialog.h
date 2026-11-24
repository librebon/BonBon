#pragma once
#include "io/DocIO.h"
#include <QDialog>

class Document;
class QCheckBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QGridLayout;
class QPushButton;
class ColorButton;

// File > Export > Favicon / App Icons...: favicon.ico (16 / 32 / 48), favicon.svg (vector documents),
// apple-touch-icon.png (180, padded on a background), android-chrome 192 / 512, site.webmanifest and
// the HTML <link> snippet, written into one folder.
class FaviconDialog : public QDialog {
    Q_OBJECT
public:
    FaviconDialog(const Document &doc, const QString &dir, QWidget *parent = nullptr);
    docio::FaviconOptions options() const;
    QString folder() const;
    // the preview tiles: size -> rendered image (for tests)
    QList<QPair<int, QImage>> previews() const { return m_previews; }
private:
    void updatePreview();
    const Document &m_doc;
    QCheckBox *m_ico[3];
    QCheckBox *m_svg;
    QSpinBox *m_padding;
    ColorButton *m_bgButton, *m_themeButton;
    QColor m_bg = Qt::white, m_theme = Qt::white;
    QLineEdit *m_name, *m_base, *m_dir;
    QGridLayout *m_grid;
    QLabel *m_snippet;
    QList<QPair<int, QImage>> m_previews;
};
