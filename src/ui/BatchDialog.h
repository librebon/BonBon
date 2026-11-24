#pragma once
// v0.3.0 File > Batch Process...: a three-page wizard (input, steps, output) over io/Batch.h.
#include "io/Batch.h"
#include <QPair>
#include <QPointer>
#include <QWizard>

class Document;
class QListWidget;
class QRadioButton;
class QLineEdit;
class QCheckBox;
class QStackedWidget;
class QVBoxLayout;
class QLabel;

class BatchDialog : public QWizard {
    Q_OBJECT
public:
    // `openDocs`: (name, document) of the documents open in the main window (Open Documents input).
    BatchDialog(const QVector<QPair<QString, Document *>> &openDocs, const QString &startDir, QWidget *parent = nullptr);
    batch::Script script() const;
    void setScript(const batch::Script &s);
    bool saveScript(const QString &path, QString *err = nullptr) const;
    bool loadScript(const QString &path, QString *err = nullptr);
    // Runs the current script (with a cancellable progress dialog unless lw_testing). Also used by tests.
    batch::Result runBatch();
protected:
    bool validateCurrentPage() override;
    void accept() override;
private:
    void buildInputPage();
    void buildStepsPage();
    void buildOutputPage();
    void refreshSteps(int select = -1);
    void editStep(int row);
    void addStep(int type);
    void moveStep(int delta);
    void syncUi();
    QStringList openDocNames() const;
    QVector<QPair<QString, QPointer<Document>>> m_open;
    QString m_dir;
    batch::Script m_s;
    batch::Result m_result;
    // input
    QRadioButton *m_inFiles, *m_inFolder, *m_inOpen;
    QListWidget *m_files;
    QLineEdit *m_folder, *m_patterns;
    QCheckBox *m_recursive;
    QLabel *m_inCount;
    // steps
    QListWidget *m_steps;
    QWidget *m_editorHost;
    QVBoxLayout *m_editorLay;
    // output
    QRadioButton *m_outFolder, *m_outOverwrite, *m_outBackup;
    QLineEdit *m_outDir, *m_backupDir;
    bool m_syncing = false;
};
