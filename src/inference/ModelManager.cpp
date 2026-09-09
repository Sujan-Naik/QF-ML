#include "../../include/inference/ModelManager.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

ModelManager::ModelManager(
    QObject *parent
)
    : QObject(parent)
      , m_networkManager(
          new QNetworkAccessManager(this)
      ) {
    m_networkManager->setProxy(
        QNetworkProxy::NoProxy
    );

    const QString defaultDirectory =
            QStandardPaths::writableLocation(
                QStandardPaths::AppDataLocation
            ) +
            QStringLiteral(
                "/models"
            );

    QSettings settings;

    m_storageDirectory =
            settings.value(
                QStringLiteral(
                    "models/storageDirectory"
                ),
                defaultDirectory
            )
            .toString()
            .trimmed();

    if (
        m_storageDirectory.isEmpty()
    ) {
        m_storageDirectory =
                defaultDirectory;
    }

    QDir().mkpath(
        m_storageDirectory
    );

    m_downloadProgressTimer =
            new QTimer(
                this
            );

    m_downloadProgressTimer
            ->setInterval(
                250
            );

    connect(
        m_downloadProgressTimer,
        &QTimer::timeout,
        this,
        &ModelManager::updateDownloadProgress
    );

    loadSelectedModel();
}

ModelManager::~ModelManager() {
    cancelDownload();

    for (
        QNetworkReply *reply:
        m_variantSizeReplies.keys()
    ) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }

    m_variantSizeReplies.clear();
}

QString ModelManager::storageDirectory() const {
    return m_storageDirectory;
}

bool ModelManager::setStorageDirectory(
    const QString &directory
) {
    const QString cleaned =
            directory.trimmed();

    if (
        cleaned.isEmpty()
    ) {
        return false;
    }

    QDir dir(
        cleaned
    );

    if (!dir.exists()) {
        if (
            !QDir().mkpath(
                cleaned
            )
        ) {
            return false;
        }
    }

    QFileInfo info(
        cleaned
    );

    if (
        !info.exists() ||
        !info.isDir() ||
        !info.isWritable()
    ) {
        return false;
    }

    const QString absolutePath =
            info.absoluteFilePath();

    if (
        absolutePath ==
        m_storageDirectory
    ) {
        return true;
    }

    m_storageDirectory =
            absolutePath;

    QSettings settings;

    settings.setValue(
        QStringLiteral(
            "models/storageDirectory"
        ),
        m_storageDirectory
    );

    loadSelectedModel();

    emit storageDirectoryChanged(
        m_storageDirectory
    );

    emit selectedModelChanged(
        m_selectedModelId
    );

    return true;
}

void ModelManager::searchRemoteLlmModels(
    const QString &query,
    int limit
) {
    if (m_searchReply) {
        m_searchReply->abort();
        m_searchReply->deleteLater();
        m_searchReply = nullptr;
    }

    const int safeLimit =
            qBound(
                1,
                limit,
                100
            );

    QUrl url(
        QStringLiteral(
            "https://huggingface.co/api/models"
        )
    );

    QUrlQuery queryParams;

    const QString cleanedQuery =
            query.trimmed();

    if (
        !cleanedQuery.isEmpty()
    ) {
        queryParams.addQueryItem(
            QStringLiteral(
                "search"
            ),
            cleanedQuery
        );
    }

    queryParams.addQueryItem(
        QStringLiteral(
            "filter"
        ),
        QStringLiteral(
            "gguf"
        )
    );

    queryParams.addQueryItem(
        QStringLiteral(
            "sort"
        ),
        QStringLiteral(
            "downloads"
        )
    );

    queryParams.addQueryItem(
        QStringLiteral(
            "direction"
        ),
        QStringLiteral(
            "-1"
        )
    );

    queryParams.addQueryItem(
        QStringLiteral(
            "limit"
        ),
        QString::number(
            safeLimit
        )
    );

    url.setQuery(
        queryParams
    );

    QNetworkRequest request(
        url
    );

    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral(
            "QF-ML/1.0"
        )
    );

    request.setTransferTimeout(
        15000
    );

    m_searchReply =
            m_networkManager->get(
                request
            );

    connect(
        m_searchReply,
        &QNetworkReply::finished,
        this,
        &ModelManager::onSearchFinished
    );
}

void ModelManager::inspectRemoteModel(
    const QString &repoId
) {
    const QString cleanedRepoId =
            repoId.trimmed();

    if (
        cleanedRepoId.isEmpty()
    ) {
        return;
    }

    if (m_inspectReply) {
        m_inspectReply->abort();
        m_inspectReply->deleteLater();
        m_inspectReply = nullptr;
    }

    QUrl url(
        QStringLiteral(
            "https://huggingface.co/api/models/%1"
        ).arg(
            cleanedRepoId
        )
    );

    QUrlQuery query;

    query.addQueryItem(
        QStringLiteral(
            "expand[]"
        ),
        QStringLiteral(
            "siblings"
        )
    );

    url.setQuery(
        query
    );

    QNetworkRequest request(
        url
    );

    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral(
            "QF-ML/1.0"
        )
    );

    request.setTransferTimeout(
        15000
    );

    m_inspectingRepoId =
            cleanedRepoId;

    m_inspectReply =
            m_networkManager->get(
                request
            );

    connect(
        m_inspectReply,
        &QNetworkReply::finished,
        this,
        &ModelManager::onInspectFinished
    );
}

QList<ModelManager::RemoteModel>
ModelManager::remoteModels() const {
    return m_remoteModels;
}

QList<ModelManager::ModelVariant>
ModelManager::remoteVariants(
    const QString &repoId
) const {
    return m_remoteVariants.value(
        repoId
    );
}

bool ModelManager::parseRemoteModel(
    const QJsonObject &object,
    RemoteModel &model
) const {
    const QString id =
            object.value(
                QStringLiteral(
                    "id"
                )
            ).toString();

    if (
        id.isEmpty()
    ) {
        return false;
    }

    model.id =
            id;

    model.author =
            object.value(
                QStringLiteral(
                    "author"
                )
            ).toString();

    if (
        model.author.isEmpty()
    ) {
        model.author =
                id.section(
                    QLatin1Char('/'),
                    0,
                    0
                );
    }

    model.displayName =
            id.section(
                QLatin1Char('/'),
                1
            );

    model.downloads =
            static_cast<qint64>(
                object.value(
                    QStringLiteral(
                        "downloads"
                    )
                ).toDouble()
            );

    model.likes =
            static_cast<qint64>(
                object.value(
                    QStringLiteral(
                        "likes"
                    )
                ).toDouble()
            );

    model.lastModified =
            object.value(
                QStringLiteral(
                    "lastModified"
                )
            ).toString();

    const QJsonArray tags =
            object.value(
                QStringLiteral(
                    "tags"
                )
            ).toArray();

    for (
        const QJsonValue &tag:
        tags
    ) {
        const QString tagString =
                tag.toString();

        if (
            !tagString.isEmpty()
        ) {
            model.tags.append(
                tagString
            );
        }
    }

    return true;
}

QList<ModelManager::ModelVariant>
ModelManager::parseVariants(
    const QString &repoId,
    const QJsonObject &object
) const {
    struct Group {
        QString quantization;
        QStringList files;
    };

    QHash<
        QString,
        Group
    > groups;

    const QJsonArray siblings =
            object.value(
                QStringLiteral(
                    "siblings"
                )
            ).toArray();

    for (
        const QJsonValue &value:
        siblings
    ) {
        const QJsonObject file =
                value.toObject();

        const QString fileName =
                file.value(
                    QStringLiteral(
                        "rfilename"
                    )
                ).toString();

        if (
            !fileName.endsWith(
                QStringLiteral(
                    ".gguf"
                ),
                Qt::CaseInsensitive
            )
        ) {
            continue;
        }

        if (
            fileName.contains(
                QStringLiteral(
                    "mmproj"
                ),
                Qt::CaseInsensitive
            )
        ) {
            continue;
        }

        const QString quantization =
                detectQuantization(
                    fileName
                );

        if (
            quantization.isEmpty()
        ) {
            continue;
        }

        Group &group =
                groups[
                    quantization
                ];

        group.quantization =
                quantization;

        group.files.append(
            fileName
        );
    }

    QList<ModelVariant> variants;

    for (
        auto iterator =
                groups.constBegin();
        iterator != groups.constEnd();
        ++iterator
    ) {
        const Group &group =
                iterator.value();

        QStringList files =
                group.files;

        std::sort(
            files.begin(),
            files.end(),
            [](const QString &left,
               const QString &right) {
                return left < right;
            }
        );

        ModelVariant variant;

        variant.repoId =
                repoId;

        variant.quantization =
                group.quantization;

        variant.fileNames =
                files;

        variant.displayName =
                QStringLiteral(
                    "%1 — %2"
                ).arg(
                    repoId.section(
                        QLatin1Char('/'),
                        1
                    ),
                    group.quantization
                );

        variant.id =
                variantId(
                    repoId,
                    files
                );

        variant.sizeBytes =
                0;

        variant.estimatedVramGb =
                0.0;

        variants.append(
            variant
        );
    }

    std::sort(
        variants.begin(),
        variants.end(),
        [](const ModelVariant &left,
           const ModelVariant &right) {
            return left.quantization >
                   right.quantization;
        }
    );

    return variants;
}

void ModelManager::onSearchFinished() {
    if (!m_searchReply)
        return;

    QNetworkReply *reply =
            m_searchReply;

    m_searchReply =
            nullptr;

    const QByteArray data =
            reply->readAll();

    const bool success =
            reply->error() ==
            QNetworkReply::NoError;

    if (!success) {
        m_remoteModels.clear();

        reply->deleteLater();

        emit remoteModelsChanged();

        return;
    }

    const QJsonDocument document =
            QJsonDocument::fromJson(
                data
            );

    reply->deleteLater();

    if (
        !document.isArray()
    ) {
        m_remoteModels.clear();

        emit remoteModelsChanged();

        return;
    }

    QList<RemoteModel> results;

    for (
        const QJsonValue &value:
        document.array()
    ) {
        RemoteModel model;

        if (
            parseRemoteModel(
                value.toObject(),
                model
            )
        ) {
            results.append(
                model
            );
        }
    }

    m_remoteModels =
            results;

    emit remoteModelsChanged();
}

void ModelManager::onInspectFinished() {
    if (!m_inspectReply)
        return;

    QNetworkReply *reply =
            m_inspectReply;

    m_inspectReply =
            nullptr;

    const QString repoId =
            m_inspectingRepoId;

    m_inspectingRepoId.clear();

    const QByteArray data =
            reply->readAll();

    const bool success =
            reply->error() ==
            QNetworkReply::NoError;

    if (!success) {
        reply->deleteLater();

        m_remoteVariants.remove(
            repoId
        );

        emit remoteVariantsChanged(
            repoId
        );

        return;
    }

    const QJsonDocument document =
            QJsonDocument::fromJson(
                data
            );

    reply->deleteLater();

    if (
        !document.isObject()
    ) {
        m_remoteVariants.remove(
            repoId
        );

        emit remoteVariantsChanged(
            repoId
        );

        return;
    }

    const QList<ModelVariant> variants =
            parseVariants(
                repoId,
                document.object()
            );

    m_remoteVariants.insert(
        repoId,
        variants
    );

    emit remoteVariantsChanged(
        repoId
    );

    resolveVariantSizes(
        repoId
    );
}

void ModelManager::resolveVariantSizes(
    const QString &repoId
) {
    const QList<ModelVariant> variants =
            m_remoteVariants.value(
                repoId
            );

    for (
        const ModelVariant &variant:
        variants
    ) {
        for (
            const QString &fileName:
            variant.fileNames
        ) {
            const QUrl url(
                resolveUrl(
                    repoId,
                    fileName
                )
            );

            QNetworkRequest request(
                url
            );

            request.setHeader(
                QNetworkRequest::UserAgentHeader,
                QStringLiteral(
                    "QF-ML/1.0"
                )
            );

            request.setTransferTimeout(
                10000
            );

            QNetworkReply *reply =
                    m_networkManager->head(
                        request
                    );

            m_variantSizeReplies.insert(
                reply,
                qMakePair(
                    repoId,
                    fileName
                )
            );

            connect(
                reply,
                &QNetworkReply::finished,
                this,
                &ModelManager::onVariantFileHeadFinished
            );
        }
    }
}

void ModelManager::onVariantFileHeadFinished() {
    QNetworkReply *reply =
            qobject_cast<QNetworkReply *>(
                sender()
            );

    if (!reply)
        return;

    const auto iterator =
            m_variantSizeReplies.find(
                reply
            );

    if (
        iterator ==
        m_variantSizeReplies.end()
    ) {
        reply->deleteLater();
        return;
    }

    const QString repoId =
            iterator.value().first;

    const QString fileName =
            iterator.value().second;

    m_variantSizeReplies.erase(
        iterator
    );

    qint64 size =
            reply->header(
                QNetworkRequest::ContentLengthHeader
            ).toLongLong();

    if (
        size <= 0
    ) {
        const QVariant contentLength =
                reply->header(
                    QNetworkRequest::ContentLengthHeader
                );

        if (
            contentLength.isValid()
        ) {
            size =
                    contentLength.toLongLong();
        }
    }

    reply->deleteLater();

    if (
        size <= 0
    ) {
        return;
    }

    updateVariantSize(
        repoId,
        fileName,
        size
    );
}

void ModelManager::updateVariantSize(
    const QString &repoId,
    const QString &fileName,
    qint64 size
) {
    QList<ModelVariant> variants =
            m_remoteVariants.value(
                repoId
            );

    bool changed =
            false;

    for (
        ModelVariant &variant:
        variants
    ) {
        if (
            !variant.fileNames.contains(
                fileName
            )
        ) {
            continue;
        }

        variant.sizeBytes +=
                size;

        variant.estimatedVramGb =
                estimateVramGb(
                    variant.sizeBytes
                );

        changed =
                true;
    }

    if (!changed)
        return;

    m_remoteVariants.insert(
        repoId,
        variants
    );

    emit remoteVariantsChanged(
        repoId
    );
}

bool ModelManager::isVariantInstalled(
    const ModelVariant &variant
) const {
    if (
        variant.id.isEmpty() ||
        variant.fileNames.isEmpty()
    ) {
        return false;
    }

    const QString directory =
            variantDirectory(
                variant
            );

    for (
        const QString &fileName:
        variant.fileNames
    ) {
        const QString path =
                QDir(
                    directory
                ).filePath(
                    fileName
                );

        QFileInfo info(
            path
        );

        if (
            !info.exists() ||
            !info.isFile() ||
            info.size() <= 0
        ) {
            return false;
        }
    }

    return true;
}

QString ModelManager::variantDirectory(
    const ModelVariant &variant
) const {
    const QByteArray source =
    (
        variant.repoId +
        QLatin1Char('|') +
        variant.quantization +
        QLatin1Char('|') +
        variant.fileNames.join(
            QLatin1Char('|')
        )
    ).toUtf8();

    const QByteArray hash =
            QCryptographicHash::hash(
                source,
                QCryptographicHash::Sha1
            );

    return QDir(
        m_storageDirectory
    ).filePath(
        QStringLiteral(
            "llm/"
        ) +
        QString::fromLatin1(
            hash.toHex()
        )
    );
}

QString ModelManager::variantEntryPath(
    const ModelVariant &variant
) const {
    if (
        variant.fileNames.isEmpty()
    ) {
        return QString();
    }

    return QDir(
        variantDirectory(
            variant
        )
    ).filePath(
        variant.fileNames.first()
    );
}

QString ModelManager::selectedModelId() const {
    return m_selectedModelId;
}

ModelManager::ModelVariant
ModelManager::selectedModel() const {
    return m_selectedModel;
}

bool ModelManager::selectModel(
    const ModelVariant &variant
) {
    if (
        variant.id.isEmpty() ||
        !isVariantInstalled(
            variant
        )
    ) {
        return false;
    }

    m_selectedModel =
            variant;

    m_selectedModelId =
            variant.id;

    saveSelectedModel(
        variant
    );

    emit selectedModelChanged(
        m_selectedModelId
    );

    return true;
}

bool ModelManager::isDownloading() const {
    return m_downloadProcess != nullptr;
}

QString ModelManager::downloadingModelId() const {
    return m_downloadingVariant.id;
}

QString ModelManager::hfExecutable() const {
    QString executable =
            QStandardPaths::findExecutable(
                QStringLiteral(
                    "hf"
                )
            );

    if (!executable.isEmpty())
        return executable;

    const QString localBin =
            QDir(
                QDir::homePath()
            ).filePath(
                QStringLiteral(
                    ".local/bin/hf"
                )
            );

    QFileInfo localBinInfo(
        localBin
    );

    if (
        localBinInfo.exists() &&
        localBinInfo.isFile() &&
        localBinInfo.isExecutable()
    ) {
        return localBinInfo.absoluteFilePath();
    }

    return QString();
}

void ModelManager::downloadModel(
    const ModelVariant &variant
) {
    if (
        m_downloadProcess ||
        variant.id.isEmpty() ||
        variant.fileNames.isEmpty()
    ) {
        return;
    }

    const QString hf =
            hfExecutable();

    if (hf.isEmpty()) {
        emit downloadError(
            variant.id,
            QStringLiteral(
                "The Hugging Face CLI 'hf' was not found."
            )
        );

        return;
    }

    const QString directory =
            variantDirectory(
                variant
            );

    if (
        !QDir().mkpath(
            directory
        )
    ) {
        emit downloadError(
            variant.id,
            QStringLiteral(
                "Could not create model directory."
            )
        );

        return;
    }

    m_downloadingVariant =
            variant;

    m_downloadFiles =
            variant.fileNames;

    m_downloadDirectory =
            directory;

    m_downloadCompletedBytes =
            0;

    m_downloadTotalBytes =
            variant.sizeBytes;

    m_downloadCancelled =
            false;

    m_downloadProcess =
            new QProcess(
                this
            );

    m_downloadProcess
            ->setProgram(
                hf
            );

    QStringList arguments;

    arguments
            << QStringLiteral(
                "download"
            )
            << variant.repoId;

    for (
        const QString &fileName:
        variant.fileNames
    ) {
        arguments
                << fileName;
    }

    arguments
            << QStringLiteral(
                "--local-dir"
            )
            << directory
            << QStringLiteral(
                "--max-workers"
            )
            << QStringLiteral(
                "8"
            );

    m_downloadProcess
            ->setArguments(
                arguments
            );

    QProcessEnvironment environment =
            QProcessEnvironment::systemEnvironment();

    environment.insert(
        QStringLiteral(
            "HF_HUB_DOWNLOAD_TIMEOUT"
        ),
        QStringLiteral(
            "1800"
        )
    );

    environment.remove(
        QStringLiteral(
            "HF_HUB_DISABLE_XET"
        )
    );

    environment.remove(
        QStringLiteral(
            "HF_HUB_ENABLE_HF_TRANSFER"
        )
    );

    m_downloadProcess
            ->setProcessEnvironment(
                environment
            );

    connect(
        m_downloadProcess,
        qOverload<int, QProcess::ExitStatus>(
            &QProcess::finished
        ),
        this,
        &ModelManager::onDownloadProcessFinished
    );

    connect(
        m_downloadProcess,
        &QProcess::errorOccurred,
        this,
        [this](
            QProcess::ProcessError error
        ) {
            if (
                !m_downloadProcess
            ) {
                return;
            }

            if (
                error ==
                QProcess::FailedToStart
            ) {
                const QString modelId =
                        m_downloadingVariant.id;

                clearDownloadState();

                emit downloadError(
                    modelId,
                    QStringLiteral(
                        "The Hugging Face CLI could not be started."
                    )
                );
            }
        }
    );

    emit downloadStarted(
        variant.id
    );

    m_downloadProcess
            ->start();

    if (
        m_downloadProgressTimer &&
        !m_downloadProgressTimer
        ->isActive()
    ) {
        m_downloadProgressTimer
                ->start();
    }
}

qint64 ModelManager::localDownloadBytes() const {
    qint64 total =
            0;

    const QDir directory(
        m_downloadDirectory
    );

    for (
        const QString &fileName:
        m_downloadFiles
    ) {
        const QFileInfo info(
            directory.filePath(
                fileName
            )
        );

        if (
            info.exists() &&
            info.isFile()
        ) {
            total +=
                    info.size();
        }
    }

    return total;
}

void ModelManager::updateDownloadProgress() {
    if (
        !m_downloadProcess ||
        m_downloadDirectory.isEmpty()
    ) {
        return;
    }

    const qint64 received =
            localDownloadBytes();

    m_downloadCompletedBytes =
            received;

    qint64 total =
            m_downloadTotalBytes;

    if (
        total <= 0
    ) {
        total =
                received;
    }

    emit downloadProgress(
        m_downloadingVariant.id,
        received,
        total
    );
}

void ModelManager::cancelDownload() {
    if (
        !m_downloadProcess
    ) {
        m_downloadCancelled =
                true;

        return;
    }

    m_downloadCancelled =
            true;

    m_downloadProcess
            ->terminate();

    if (
        !m_downloadProcess
        ->waitForFinished(
            2000
        )
    ) {
        m_downloadProcess
                ->kill();
    }
}

void ModelManager::onDownloadProcessFinished(
    int exitCode,
    QProcess::ExitStatus exitStatus
) {
    if (
        !m_downloadProcess
    ) {
        return;
    }

    QProcess *process =
            m_downloadProcess;

    m_downloadProcess =
            nullptr;

    if (
        m_downloadProgressTimer
    ) {
        m_downloadProgressTimer
                ->stop();
    }

    const QString modelId =
            m_downloadingVariant.id;

    const ModelVariant completed =
            m_downloadingVariant;

    const QString standardError =
            QString::fromUtf8(
                process
                ->readAllStandardError()
            ).trimmed();

    const QString standardOutput =
            QString::fromUtf8(
                process
                ->readAllStandardOutput()
            ).trimmed();

    process->deleteLater();

    if (
        m_downloadCancelled
    ) {
        clearDownloadState();

        emit downloadError(
            modelId,
            QStringLiteral(
                "Download cancelled."
            )
        );

        return;
    }

    if (
        exitStatus !=
        QProcess::NormalExit ||
        exitCode != 0
    ) {
        QString error =
                standardError;

        if (
            error.isEmpty()
        ) {
            error =
                    standardOutput;
        }

        if (
            error.isEmpty()
        ) {
            error =
                    QStringLiteral(
                        "Hugging Face download failed with exit code %1."
                    ).arg(
                        exitCode
                    );
        }

        clearDownloadState();

        emit downloadError(
            modelId,
            error
        );

        return;
    }

    if (
        !isVariantInstalled(
            completed
        )
    ) {
        clearDownloadState();

        emit downloadError(
            modelId,
            QStringLiteral(
                "Hugging Face reported success, but the expected model files were not installed."
            )
        );

        return;
    }

    emit downloadProgress(
        modelId,
        completed.sizeBytes > 0
            ? completed.sizeBytes
            : localDownloadBytes(),
        completed.sizeBytes
    );

    clearDownloadState();

    selectModel(
        completed
    );

    emit downloadFinished(
        completed.id
    );
}

void ModelManager::loadSelectedModel() {
    QSettings settings;

    const QString repoId =
            settings.value(
                QStringLiteral(
                    "models/selected/repoId"
                )
            ).toString();

    const QString modelId =
            settings.value(
                QStringLiteral(
                    "models/selected/id"
                )
            ).toString();

    const QStringList files =
            settings.value(
                QStringLiteral(
                    "models/selected/files"
                )
            ).toStringList();

    if (
        repoId.isEmpty() ||
        modelId.isEmpty() ||
        files.isEmpty()
    ) {
        m_selectedModelId.clear();
        m_selectedModel =
                ModelVariant();

        return;
    }

    ModelVariant variant;

    variant.id =
            modelId;

    variant.repoId =
            repoId;

    variant.displayName =
            settings.value(
                QStringLiteral(
                    "models/selected/displayName"
                )
            ).toString();

    variant.quantization =
            settings.value(
                QStringLiteral(
                    "models/selected/quantization"
                )
            ).toString();

    variant.fileNames =
            files;

    variant.sizeBytes =
            settings.value(
                QStringLiteral(
                    "models/selected/sizeBytes"
                )
            ).toLongLong();

    variant.estimatedVramGb =
            settings.value(
                QStringLiteral(
                    "models/selected/estimatedVramGb"
                )
            ).toDouble();

    if (
        !isVariantInstalled(
            variant
        )
    ) {
        m_selectedModelId.clear();
        m_selectedModel =
                ModelVariant();

        return;
    }

    m_selectedModel =
            variant;

    m_selectedModelId =
            variant.id;
}

void ModelManager::saveSelectedModel(
    const ModelVariant &variant
) {
    QSettings settings;

    settings.setValue(
        QStringLiteral(
            "models/selected/id"
        ),
        variant.id
    );

    settings.setValue(
        QStringLiteral(
            "models/selected/repoId"
        ),
        variant.repoId
    );

    settings.setValue(
        QStringLiteral(
            "models/selected/displayName"
        ),
        variant.displayName
    );

    settings.setValue(
        QStringLiteral(
            "models/selected/quantization"
        ),
        variant.quantization
    );

    settings.setValue(
        QStringLiteral(
            "models/selected/files"
        ),
        variant.fileNames
    );

    settings.setValue(
        QStringLiteral(
            "models/selected/sizeBytes"
        ),
        variant.sizeBytes
    );

    settings.setValue(
        QStringLiteral(
            "models/selected/estimatedVramGb"
        ),
        variant.estimatedVramGb
    );
}

double ModelManager::estimateVramGb(
    qint64 sizeBytes
) {
    if (
        sizeBytes <= 0
    ) {
        return 0.0;
    }

    const double modelGb =
            static_cast<double>(
                sizeBytes
            ) /
            1000000000.0;

    return qMax(
        2.0,
        modelGb * 1.15 + 1.0
    );
}

QString ModelManager::detectQuantization(
    const QString &fileName
) {
    static const QRegularExpression regex(
        QStringLiteral(
            "(IQ[0-9]+_[A-Z0-9]+|"
            "Q[0-9]+_[A-Z0-9_]+|"
            "Q[0-9]+|"
            "F32|F16|BF16|FP16|FP8|"
            "NVFP4|MXFP4)"
        ),
        QRegularExpression::CaseInsensitiveOption
    );

    const QRegularExpressionMatch match =
            regex.match(
                fileName
            );

    if (
        !match.hasMatch()
    ) {
        return QString();
    }

    return match.captured(
        1
    ).toUpper();
}

QString ModelManager::variantId(
    const QString &repoId,
    const QStringList &fileNames
) {
    const QByteArray source =
    (
        repoId +
        QLatin1Char('|') +
        fileNames.join(
            QLatin1Char('|')
        )
    ).toUtf8();

    return QString::fromLatin1(
        QCryptographicHash::hash(
            source,
            QCryptographicHash::Sha1
        ).toHex()
    );
}

QString ModelManager::resolveUrl(
    const QString &repoId,
    const QString &fileName
) {
    QString encodedFileName;

    const QStringList parts =
            fileName.split(
                QLatin1Char('/'),
                Qt::SkipEmptyParts
            );

    for (
        int index = 0;
        index < parts.size();
        ++index
    ) {
        if (
            index > 0
        ) {
            encodedFileName +=
                    QLatin1Char('/');
        }

        encodedFileName +=
                QString::fromLatin1(
                    QUrl::toPercentEncoding(
                        parts[index]
                    )
                );
    }

    return QStringLiteral(
        "https://huggingface.co/%1/resolve/main/%2"
    ).arg(
        repoId,
        encodedFileName
    );
}

void ModelManager::clearDownloadState() {
    m_downloadingVariant =
            ModelVariant();

    m_downloadFiles.clear();

    m_downloadDirectory.clear();

    m_downloadTotalBytes =
            0;

    m_downloadCompletedBytes =
            0;

    m_downloadCancelled =
            false;
}
