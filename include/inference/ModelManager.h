#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <QProcess>
#include <QTimer>

class QFile;

class ModelManager : public QObject {
    Q_OBJECT

public:
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
        double estimatedVramGb = 0.0;
    };

    explicit ModelManager(
        QObject *parent = nullptr
    );

    ~ModelManager() override;

    QString storageDirectory() const;

    bool setStorageDirectory(
        const QString &directory
    );

    void searchRemoteLlmModels(
        const QString &query,
        int limit = 30
    );

    void inspectRemoteModel(
        const QString &repoId
    );

    QList<RemoteModel>
    remoteModels() const;

    QList<ModelVariant>
    remoteVariants(
        const QString &repoId
    ) const;

    bool isVariantInstalled(
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

    void downloadModel(
        const ModelVariant &variant
    );

    void cancelDownload();

    bool isDownloading() const;

    QString downloadingModelId() const;

signals:
    void remoteModelsChanged();

    void remoteVariantsChanged(
        const QString &repoId
    );

    void storageDirectoryChanged(
        const QString &directory
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

private slots:
    void onSearchFinished();

    void onInspectFinished();

    void onVariantFileHeadFinished();

    void onDownloadProcessFinished(
        int exitCode,
        QProcess::ExitStatus exitStatus
    );

    void updateDownloadProgress();

private:
    bool parseRemoteModel(
        const QJsonObject &object,
        RemoteModel &model
    ) const;

    QList<ModelVariant> parseVariants(
        const QString &repoId,
        const QJsonObject &object
    ) const;

    void resolveVariantSizes(
        const QString &repoId
    );

    void updateVariantSize(
        const QString &repoId,
        const QString &fileName,
        qint64 size
    );

    QString variantDirectory(
        const ModelVariant &variant
    ) const;

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

    void loadSelectedModel();

    void saveSelectedModel(
        const ModelVariant &variant
    );

    void clearDownloadState();

    QString hfExecutable() const;

    qint64 localDownloadBytes() const;

private:
    QNetworkAccessManager *
            m_networkManager = nullptr;

    QNetworkReply *
            m_searchReply = nullptr;

    QNetworkReply *
            m_inspectReply = nullptr;

    QHash<
        QNetworkReply *,
        QPair<QString, QString>
    > m_variantSizeReplies;

    QString
            m_storageDirectory;

    QList<RemoteModel>
            m_remoteModels;

    QHash<
        QString,
        QList<ModelVariant>
    > m_remoteVariants;

    QString
            m_inspectingRepoId;

    QString
            m_selectedModelId;

    ModelVariant
            m_selectedModel;

    QProcess *
            m_downloadProcess = nullptr;

    QTimer *
            m_downloadProgressTimer = nullptr;

    ModelVariant
            m_downloadingVariant;

    QStringList
            m_downloadFiles;

    QString
            m_downloadDirectory;

    qint64
            m_downloadTotalBytes = 0;

    qint64
            m_downloadCompletedBytes = 0;

    bool
            m_downloadCancelled = false;
};
