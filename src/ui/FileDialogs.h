#pragma once
#include <QFileDialog>
#include <QString>
#include <QStringList>
class QWidget;

// All file / folder dialogs go through here so Preferences > "Use system file
// dialogs" (off by default) can force Qt's own non-native dialog — the same
// one the AppImage shows — without a restart.
namespace filedialogs {
QFileDialog::Options options(QFileDialog::Options extra = {});

// For the Qt (non-native) dialog: stop the file-type combo sizing itself to the
// longest filter string, which otherwise becomes the dialog's minimum width and
// stops the window shrinking. The closed combo stays about as wide as the
// filename field. The popup lists every filter, capped to the narrower of the
// dialog width and about 60% of the screen, with long rows elided in the middle
// and the full text on a tooltip. Safe to call when the dialog has no filter
// combo (e.g. Choose Directory).
constexpr int kFilterComboMinContentsLength = 24;
void configureFilterCombo(QFileDialog *dlg);

QString getOpenFileName(QWidget *parent, const QString &caption, const QString &dir,
                        const QString &filter = {}, QString *selectedFilter = nullptr,
                        QFileDialog::Options extra = {});
QStringList getOpenFileNames(QWidget *parent, const QString &caption, const QString &dir,
                             const QString &filter = {}, QString *selectedFilter = nullptr,
                             QFileDialog::Options extra = {});
QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir,
                        const QString &filter = {}, QString *selectedFilter = nullptr,
                        QFileDialog::Options extra = {});
QString getExistingDirectory(QWidget *parent, const QString &caption, const QString &dir,
                             QFileDialog::Options extra = QFileDialog::ShowDirsOnly);
}
