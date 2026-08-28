#pragma once

#include <QObject>
#include <QFile>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QUrl>
#include <QHash>

class ModelManager : public QObject {
    Q_OBJECT

public:
    enum class ModelType {
        Llm,
        Stt,
        Tts,
        Ocr
    };

    Q_ENUM(ModelType)

    struct RemoteModel {
        QString id;
        QString author;
        QString displayName;

        qint64 downloads = 0;
        qint64 likes = 0;

        QString lastModified;

        QStringList tags;
    };

    struct ModelVariant {
        QString id;

        QString repoId;
        QString displayName;

        QString quantization;

        QStringList fileNames;

        qint64 sizeBytes = 0;
        qint64 downloads = 0;

        QString lastModified;

        double estimatedVramGb = 0.0;
    };

    explicit ModelManager(
        QObject *parent = nullptr
    );

    ~ModelManager() override;

    // -------------------------------------------------------------------------
    // Local storage
    // -------------------------------------------------------------------------

    QString storageDirectory() const;

    bool setStorageDirectory(
        const QString &directory
    );

    // -------------------------------------------------------------------------
    // Hugging Face search
    // -------------------------------------------------------------------------

    void searchRemoteLlmModels(
        const QString &query,
        int limit = 30
    );

    void inspectRemoteModel(
        const QString &repoId
    );

    QList<RemoteModel> remoteModels() const;

    QList<ModelVariant> remoteVariants(
        const QString &repoId
    ) const;

    // -------------------------------------------------------------------------
    // Local model state
    // -------------------------------------------------------------------------

    bool isVariantInstalled(
        const ModelVariant &variant
    ) const;

    QString variantDirectory(
        const ModelVariant &variant
    ) const;

    QString variantEntryPath(
        const ModelVariant &variant
    ) const;

    QString selectedModelId() const;

    ModelVariant selectedModel() const;

    bool selectModel(
        const ModelVariant &variant
    );

    // -------------------------------------------------------------------------
    // Download
    // -------------------------------------------------------------------------

    bool isDownloading() const;

    QString downloadingModelId() const;

    void downloadModel(
        const ModelVariant &variant
    );

    void cancelDownload();

signals:
    void remoteModelsChanged();

    void remoteVariantsChanged(
        const QString &repoId
    );

    void selectedModelChanged(
        const QString &modelId
    );

    void downloadStarted(
        const QString &modelId
    );

    void downloadProgress(
        const QString &modelId,
        qint64 received,
        qint64 total
    );

    void downloadFinished(
        const QString &modelId
    );

    void downloadError(
        const QString &modelId,
        const QString &error
    );

    void storageDirectoryChanged(
        const QString &directory
    );

private slots:
    void onSearchFinished();

    void onInspectFinished();

    void onVariantFileHeadFinished();

    void onDownloadReadyRead();

    void onDownloadFinished();

private:
    static double estimateVramGb(
        qint64 sizeBytes
    );

    static QString detectQuantization(
        const QString &fileName
    );

    static QString variantId(
        const QString &repoId,
        const QStringList &fileNames
    );

    static QString resolveUrl(
        const QString &repoId,
        const QString &fileName
    );

    bool parseRemoteModel(
        const QJsonObject &object,
        RemoteModel &model
    ) const;

    QList<ModelVariant> parseVariants(
        const QString &repoId,
        const QJsonObject &object
    ) const;

    void loadSelectedModel();

    void saveSelectedModel(
        const ModelVariant &variant
    );

    void clearDownloadState();

    void closeDownloadFile();

    void startNextDownloadFile();

    void resolveVariantSizes(
        const QString &repoId
    );

    void updateVariantSize(
        const QString &repoId,
        const QString &fileName,
        qint64 size
    );

private:
    QNetworkAccessManager *m_networkManager =
            nullptr;

    QNetworkReply *m_searchReply =
            nullptr;

    QNetworkReply *m_inspectReply =
            nullptr;

    QHash<
        QNetworkReply *,
        QPair<QString, QString>
    > m_variantSizeReplies;

    QNetworkReply *m_downloadReply =
            nullptr;

    QFile *m_downloadFile =
            nullptr;

    QString m_storageDirectory;

    QList<RemoteModel> m_remoteModels;

    QHash<
        QString,
        QList<ModelVariant>
    > m_remoteVariants;

    QString m_inspectingRepoId;

    QString m_selectedModelId;

    ModelVariant m_selectedModel;

    ModelVariant m_downloadingVariant;

    QStringList m_downloadFiles;

    int m_downloadFileIndex = 0;

    qint64 m_downloadCompletedBytes = 0;

    qint64 m_downloadTotalBytes = 0;

    QString m_downloadDirectory;

    QString m_downloadPartPath;

    bool m_downloadCancelled = false;
};
