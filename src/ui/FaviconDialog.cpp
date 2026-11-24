#include "PopupSlider.h"
#include "FaviconDialog.h"
#include "core/Document.h"
#include <QCheckBox>
#include "Dialogs.h"
#include <QDialogButtonBox>
#include <QFileDialog>
#include "FileDialogs.h"
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

// a checkerboard behind transparent previews
static QPixmap tile(const QImage &img, int box) {
    QPixmap pm(box, box);
    QPainter p(&pm);
    for (int y = 0; y < box; y += 8)
        for (int x = 0; x < box; x += 8) p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(204, 204, 204) : Qt::white);
    const QImage s = img.width() > box ? img.scaled(box, box, Qt::KeepAspectRatio, Qt::SmoothTransformation) : img;
    p.drawImage((box - s.width()) / 2, (box - s.height()) / 2, s);
    return pm;
}

FaviconDialog::FaviconDialog(const Document &doc, const QString &dir, QWidget *parent) : QDialog(parent), m_doc(doc) {
    setWindowTitle(QStringLiteral("Export Favicon / App Icons"));
    setObjectName(QStringLiteral("faviconDialog"));
    auto *top = new QVBoxLayout(this);

    auto *gridBox = new QGroupBox(QStringLiteral("Preview"));
    m_grid = new QGridLayout(gridBox);
    top->addWidget(gridBox);

    auto *f = new QFormLayout;
    auto *icoRow = new QHBoxLayout;
    const int sizes[3] = {16, 32, 48};
    for (int i = 0; i < 3; ++i) {
        m_ico[i] = new QCheckBox(QStringLiteral("%1").arg(sizes[i]));
        m_ico[i]->setObjectName(QStringLiteral("faviconIco%1").arg(sizes[i]));
        m_ico[i]->setChecked(true);
        icoRow->addWidget(m_ico[i]);
        connect(m_ico[i], &QCheckBox::toggled, this, &FaviconDialog::updatePreview);
    }
    icoRow->addStretch();
    f->addRow(QStringLiteral("favicon.ico sizes:"), icoRow);
    m_svg = new QCheckBox(QStringLiteral("Write favicon.svg"));
    m_svg->setObjectName(QStringLiteral("faviconSvg"));
    const bool vec = docio::isVectorDocument(doc.s, doc.curFrame);
    m_svg->setChecked(vec);
    m_svg->setEnabled(vec);
    if (!vec) m_svg->setToolTip(QStringLiteral("Only documents made entirely of vector objects can be written as SVG"));
    f->addRow(QString(), m_svg);
    m_padding = new PopupSliderField;
    m_padding->setObjectName(QStringLiteral("faviconPadding"));
    m_padding->setRange(0, 40);
    m_padding->setSuffix(QStringLiteral(" %"));
    m_padding->setValue(10);
    connect(m_padding, &QSpinBox::valueChanged, this, &FaviconDialog::updatePreview);
    f->addRow(QStringLiteral("Apple icon padding:"), m_padding);
    m_bgButton = new ColorButton(this, true);
    m_bgButton->setObjectName(QStringLiteral("faviconBackground"));
    m_bgButton->setColor(m_bg);
    // a hovered swatch or an eyedropper sample previews live; a pick keeps it
    connect(m_bgButton, &ColorButton::colorPreview, this, [this](const QColor &c) { m_bg = c; updatePreview(); });
    connect(m_bgButton, &ColorButton::colorPicked, this, [this](const QColor &c) { m_bg = c; updatePreview(); });
    f->addRow(QStringLiteral("Apple icon background:"), m_bgButton);
    m_name = new QLineEdit(QFileInfo(doc.title()).completeBaseName());
    m_name->setObjectName(QStringLiteral("faviconAppName"));
    connect(m_name, &QLineEdit::textChanged, this, &FaviconDialog::updatePreview);
    f->addRow(QStringLiteral("App name:"), m_name);
    m_themeButton = new ColorButton(this, true);
    m_themeButton->setObjectName(QStringLiteral("faviconTheme"));
    m_themeButton->setColor(m_theme);
    connect(m_themeButton, &ColorButton::colorPicked, this, [this](const QColor &c) { m_theme = c; updatePreview(); });
    f->addRow(QStringLiteral("Theme colour:"), m_themeButton);
    m_base = new QLineEdit(QStringLiteral("/"));
    m_base->setObjectName(QStringLiteral("faviconBasePath"));
    connect(m_base, &QLineEdit::textChanged, this, &FaviconDialog::updatePreview);
    f->addRow(QStringLiteral("URL path:"), m_base);
    auto *dirRow = new QHBoxLayout;
    m_dir = new QLineEdit(dir);
    m_dir->setObjectName(QStringLiteral("faviconFolder"));
    auto *browse = new QPushButton(QStringLiteral("Browse..."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = filedialogs::getExistingDirectory(this, QStringLiteral("Export Icons To"), m_dir->text());
        if (!d.isEmpty()) m_dir->setText(d);
    });
    dirRow->addWidget(m_dir, 1);
    dirRow->addWidget(browse);
    f->addRow(QStringLiteral("Folder:"), dirRow);
    top->addLayout(f);

    m_snippet = new QLabel;
    m_snippet->setObjectName(QStringLiteral("faviconSnippet"));
    m_snippet->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_snippet->setStyleSheet(QStringLiteral("font-family: monospace;"));
    m_snippet->setWordWrap(false);
    m_snippet->setTextFormat(Qt::RichText);
    top->addWidget(m_snippet);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Export"));
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    top->addWidget(bb);
    updatePreview();
}

QString FaviconDialog::folder() const { return m_dir->text(); }

docio::FaviconOptions FaviconDialog::options() const {
    docio::FaviconOptions o;
    o.icoSizes = 0;
    for (int i = 0; i < 3; ++i) if (m_ico[i]->isChecked()) o.icoSizes |= 1 << i;
    if (!o.icoSizes) o.icoSizes = 0x2;   // an .ico needs at least one image: 32
    o.svg = m_svg->isChecked();
    o.applePadding = m_padding->value();
    o.appleBackground = m_bg;
    o.appName = m_name->text();
    o.themeColor = m_theme;
    o.basePath = m_base->text().isEmpty() ? QStringLiteral("/") : m_base->text();
    return o;
}

void FaviconDialog::updatePreview() {
    while (QLayoutItem *it = m_grid->takeAt(0)) { delete it->widget(); delete it; }
    m_previews.clear();
    const docio::FaviconOptions o = options();
    struct T { int size; QString label; bool apple; };
    QList<T> tiles;
    const int ico[3] = {16, 32, 48};
    for (int i = 0; i < 3; ++i) if (o.icoSizes & (1 << i)) tiles << T{ico[i], QStringLiteral("favicon.ico %1").arg(ico[i]), false};
    tiles << T{180, QStringLiteral("apple-touch-icon 180"), true} << T{192, QStringLiteral("android-chrome 192"), false} << T{512, QStringLiteral("android-chrome 512"), false};
    int col = 0;
    for (const T &t : tiles) {
        const QImage img = docio::faviconImage(m_doc.s, m_doc.curFrame, t.size, t.apple ? o.applePadding : 0, t.apple ? o.appleBackground : QColor(Qt::transparent));
        m_previews << qMakePair(t.size, img);
        auto *pic = new QLabel;
        pic->setPixmap(tile(img, t.size <= 48 ? 48 : 96));
        pic->setAlignment(Qt::AlignCenter);
        pic->setToolTip(QStringLiteral("%1 x %1").arg(t.size));
        auto *cap = new QLabel(t.label);
        cap->setAlignment(Qt::AlignCenter);
        m_grid->addWidget(pic, 0, col);
        m_grid->addWidget(cap, 1, col);
        ++col;
    }
    const QString b = o.basePath.endsWith(QLatin1Char('/')) ? o.basePath : o.basePath + QLatin1Char('/');
    QStringList l;
    l << QStringLiteral("<link rel=\"icon\" href=\"%1favicon.ico\" sizes=\"any\">").arg(b);
    if (o.svg && m_svg->isEnabled()) l << QStringLiteral("<link rel=\"icon\" href=\"%1favicon.svg\" type=\"image/svg+xml\">").arg(b);
    l << QStringLiteral("<link rel=\"apple-touch-icon\" href=\"%1apple-touch-icon.png\">").arg(b)
      << QStringLiteral("<link rel=\"manifest\" href=\"%1site.webmanifest\">").arg(b);
    m_snippet->setText(l.join(QLatin1Char('\n')).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
}
