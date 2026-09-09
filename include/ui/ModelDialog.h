#ifndef MODELDIALOG_H
#define MODELDIALOG_H

#include <QDialog>
#include <QString>

class InferenceService;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;

class ModelDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModelDialog(
        InferenceService *inferenceService,
        QWidget *parent = nullptr
    );
    ~ModelDialog() override = default;

private slots:
    void chooseDirectory();
    void search();
    void recommended();
    void openQuantizationGuide();
    void repositorySelectionChanged();
    void variantSelectionChanged();
    void localModelSelectionChanged();
    void refreshLocalModels();
    void downloadSelected();
    void selectSelected();

    void onRemoteModelsChanged();
    void onRemoteVariantsChanged(const QString &repoId);
    void onDownloadStarted(const QString &modelId);
    void onDownloadProgress(
        const QString &modelId,
        qint64 received,
        qint64 total
    );
    void onDownloadFinished(const QString &modelId);
    void onDownloadError(
        const QString &modelId,
        const QString &error
    );

private:
    void populateLocalModels();
    void populateRepositories();
    void populateVariants();
    void updateLocalModelInfo();
    void updateVariantInfo();
    void updateButtons();

    bool selectLocalModelPath(const QString &path);

    QString selectedRepoId() const;
    QString selectedVariantId() const;
    QString selectedLocalModelPath() const;

    InferenceService *m_inference{nullptr};

    QLabel *m_directoryLabel{nullptr};
    QPushButton *m_directoryButton{nullptr};

    QPushButton *m_refreshLocalButton{nullptr};
    QListWidget *m_localModelList{nullptr};
    QLabel *m_localModelInfoLabel{nullptr};

    QLineEdit *m_searchEdit{nullptr};
    QPushButton *m_searchButton{nullptr};
    QPushButton *m_recommendedButton{nullptr};
    QPushButton *m_quantizationGuideButton{nullptr};
    QComboBox *m_vramFilter{nullptr};

    QListWidget *m_repositoryList{nullptr};
    QListWidget *m_variantList{nullptr};

    QLabel *m_modelInfoLabel{nullptr};
    QLabel *m_statusLabel{nullptr};
    QProgressBar *m_progress{nullptr};

    QPushButton *m_downloadButton{nullptr};
    QPushButton *m_selectButton{nullptr};
};

#endif // MODELDIALOG_H