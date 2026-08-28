#pragma once

#include <QDialog>

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

private slots:
    void search();

    void recommended();

    void openQuantizationGuide();

    void chooseDirectory();

    void repositorySelectionChanged();

    void variantSelectionChanged();

    void downloadSelected();

    void selectSelected();

    void onRemoteModelsChanged();

    void onRemoteVariantsChanged(
        const QString &repoId
    );

    void onDownloadStarted(
        const QString &modelId
    );

    void onDownloadProgress(
        const QString &modelId,
        qint64 received,
        qint64 total
    );

    void onDownloadFinished(
        const QString &modelId
    );

    void onDownloadError(
        const QString &modelId,
        const QString &error
    );

private:
    QString selectedRepoId() const;

    QString selectedVariantId() const;

    void populateRepositories();

    void populateVariants();

    void updateVariantInfo();

    void updateButtons();

private:
    InferenceService *
            m_inference = nullptr;

    QLineEdit *
            m_searchEdit = nullptr;

    QComboBox *
            m_vramFilter = nullptr;

    QPushButton *
            m_searchButton = nullptr;

    QPushButton *
            m_recommendedButton = nullptr;

    QPushButton *
            m_quantizationGuideButton = nullptr;

    QPushButton *
            m_directoryButton = nullptr;

    QLabel *
            m_directoryLabel = nullptr;

    QListWidget *
            m_repositoryList = nullptr;

    QListWidget *
            m_variantList = nullptr;

    QLabel *
            m_modelInfoLabel = nullptr;

    QLabel *
            m_statusLabel = nullptr;

    QProgressBar *
            m_progress = nullptr;

    QPushButton *
            m_downloadButton = nullptr;

    QPushButton *
            m_selectButton = nullptr;
};
