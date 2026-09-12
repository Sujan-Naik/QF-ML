#include "inference/ModelManager.h"
#include "app/QfPaths.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrlQuery>

ModelManager::ModelManager(QObject *parent)
    : QObject(parent), m_networkManager(new QNetworkAccessManager(this)),
      m_downloadProgressTimer(new QTimer(this)) {

  m_storageDirectory = QFPaths::llmModelsDir();

  connect(m_downloadProgressTimer, &QTimer::timeout, this,
          &ModelManager::updateDownloadProgress);
}

ModelManager::~ModelManager() = default;

QString ModelManager::storageDirectory() const {
  return m_storageDirectory.isEmpty() ? QFPaths::llmModelsDir()
                                      : m_storageDirectory;
}

bool ModelManager::setStorageDirectory(const QString &directory) {
  const QString targetDir =
      directory.isEmpty() ? QFPaths::llmModelsDir() : directory;

  if (m_storageDirectory == targetDir)
    return true;

  QDir dir(targetDir);
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    return false;
  }

  m_storageDirectory = targetDir;
  loadSelectedModel();
  emit storageDirectoryChanged(m_storageDirectory);
  return true;
}

void ModelManager::searchRemoteLlmModels(const QString &query, int limit) {
  if (m_searchReply) {
    m_searchReply->abort();
    m_searchReply->deleteLater();
    m_searchReply = nullptr;
  }

  QUrl url(QStringLiteral("https://huggingface.co/api/models"));
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("search"), query);
  q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
  q.addQueryItem(QStringLiteral("filter"), QStringLiteral("gguf"));
  url.setQuery(q);

  QNetworkRequest request(url);
  m_searchReply = m_networkManager->get(request);

  connect(m_searchReply, &QNetworkReply::finished, this,
          &ModelManager::onSearchFinished);
}

void ModelManager::inspectRemoteModel(const QString &repoId) {
  if (m_inspectReply) {
    m_inspectReply->abort();
    m_inspectReply->deleteLater();
    m_inspectReply = nullptr;
  }

  m_inspectingRepoId = repoId;
  QUrl url(QStringLiteral("https://huggingface.co/api/models/%1").arg(repoId));
  QNetworkRequest request(url);
  m_inspectReply = m_networkManager->get(request);

  connect(m_inspectReply, &QNetworkReply::finished, this,
          &ModelManager::onInspectFinished);
}

QList<ModelManager::RemoteModel> ModelManager::remoteModels() const {
  return m_remoteModels;
}

QList<ModelManager::ModelVariant>
ModelManager::remoteVariants(const QString &repoId) const {
  return m_remoteVariants.value(repoId);
}

bool ModelManager::isVariantInstalled(const ModelVariant &variant) const {
  const QString path = variantEntryPath(variant);
  if (path.isEmpty())
    return false;

  QFileInfo info(path);
  return info.exists() && info.isFile() && info.size() > 0;
}

QString ModelManager::variantEntryPath(const ModelVariant &variant) const {
  if (variant.fileNames.isEmpty())
    return QString();

  const QString dirPath = variantDirectory(variant);
  return QDir(dirPath).filePath(variant.fileNames.first());
}

QString ModelManager::selectedModelId() const { return m_selectedModelId; }

ModelManager::ModelVariant ModelManager::selectedModel() const {
  return m_selectedModel;
}

bool ModelManager::selectModel(const ModelVariant &variant) {
  if (m_selectedModelId == variant.id)
    return true;

  m_selectedModel = variant;
  m_selectedModelId = variant.id;

  saveSelectedModel(variant);
  emit selectedModelChanged(m_selectedModelId);
  return true;
}

bool ModelManager::selectModel(const QString &modelPathOrId) {
  if (m_selectedModelId == modelPathOrId)
    return true;

  m_selectedModelId = modelPathOrId;
  m_selectedModel = ModelVariant{};
  m_selectedModel.id = modelPathOrId;
  m_selectedModel.displayName = QFileInfo(modelPathOrId).fileName();

  saveSelectedModel(m_selectedModel);
  emit selectedModelChanged(m_selectedModelId);
  return true;
}

void ModelManager::downloadModel(const ModelVariant &variant) {
  if (isDownloading()) {
    emit downloadError(variant.id,
                       QStringLiteral("A download is already in progress."));
    return;
  }

  m_downloadingVariant = variant;
  m_downloadFiles = variant.fileNames;
  m_downloadDirectory = variantDirectory(variant);

  m_downloadTotalBytes = variant.sizeBytes;
  if (m_downloadTotalBytes <= 0 && m_remoteVariants.contains(variant.repoId)) {
    for (const auto &v : m_remoteVariants[variant.repoId]) {
      if (v.id == variant.id) {
        m_downloadTotalBytes = v.sizeBytes;
        break;
      }
    }
  }

  m_downloadCompletedBytes = 0;
  m_downloadCancelled = false;

  QDir dir;
  if (!dir.mkpath(m_downloadDirectory)) {
    emit downloadError(
        variant.id,
        QStringLiteral("Failed to create target download directory."));
    return;
  }

  const QString executable = hfExecutable();
  if (executable.isEmpty()) {
    emit downloadError(variant.id,
                       QStringLiteral("HuggingFace CLI executable not found."));
    return;
  }

  m_downloadProcess = new QProcess(this);
  connect(m_downloadProcess,
          QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          &ModelManager::onDownloadProcessFinished);

  QStringList args;
  args << QStringLiteral("download") << variant.repoId;

  for (const QString &fn : variant.fileNames) {
    args << fn;
  }

  args << QStringLiteral("--local-dir") << m_downloadDirectory;

  m_downloadProcess->start(executable, args);
  m_downloadProgressTimer->start(500);

  emit downloadStarted(variant.id);
}

void ModelManager::cancelDownload() {
  if (!isDownloading())
    return;

  m_downloadCancelled = true;
  if (m_downloadProcess) {
    m_downloadProcess->kill();
  }
}

bool ModelManager::isDownloading() const {
  return m_downloadProcess != nullptr;
}

QString ModelManager::downloadingModelId() const {
  return m_downloadingVariant.id;
}

void ModelManager::onSearchFinished() {
  if (!m_searchReply)
    return;

  m_remoteModels.clear();

  if (m_searchReply->error() == QNetworkReply::NoError) {
    const QByteArray data = m_searchReply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isArray()) {
      const QJsonArray array = doc.array();
      for (const QJsonValue &val : array) {
        if (val.isObject()) {
          RemoteModel model;
          if (parseRemoteModel(val.toObject(), model)) {
            m_remoteModels.append(model);
          }
        }
      }
    }
  }

  m_searchReply->deleteLater();
  m_searchReply = nullptr;

  emit remoteModelsChanged();
}

void ModelManager::onInspectFinished() {
  if (!m_inspectReply)
    return;

  const QString repoId = m_inspectingRepoId;

  if (m_inspectReply->error() == QNetworkReply::NoError) {
    const QByteArray data = m_inspectReply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isObject()) {
      QList<ModelVariant> variants = parseVariants(repoId, doc.object());
      m_remoteVariants.insert(repoId, variants);
      resolveVariantSizes(repoId);
    }
  }

  m_inspectReply->deleteLater();
  m_inspectReply = nullptr;

  emit remoteVariantsChanged(repoId);
}

void ModelManager::onVariantFileHeadFinished() {
  auto reply = qobject_cast<QNetworkReply *>(sender());
  if (!reply)
    return;

  if (m_variantSizeReplies.contains(reply)) {
    const auto pair = m_variantSizeReplies.take(reply);
    const QString repoId = pair.first;
    const QString fileName = pair.second;

    if (reply->error() == QNetworkReply::NoError) {
      const QString contentLength =
          reply->header(QNetworkRequest::ContentLengthHeader).toString();
      bool ok = false;
      const qint64 size = contentLength.toLongLong(&ok);
      if (ok && size > 0) {
        updateVariantSize(repoId, fileName, size);
      }
    }
  }

  reply->deleteLater();
}

void ModelManager::onDownloadProcessFinished(int exitCode,
                                             QProcess::ExitStatus exitStatus) {
  m_downloadProgressTimer->stop();

  const QString modelId = m_downloadingVariant.id;

  if (m_downloadCancelled) {
    emit downloadError(modelId, QStringLiteral("Download was cancelled."));
  } else if (exitStatus == QProcess::NormalExit && exitCode == 0) {
    emit downloadFinished(modelId);
  } else {
    const QString err = m_downloadProcess
                            ? m_downloadProcess->readAllStandardError()
                            : QString();
    emit downloadError(modelId, err.isEmpty()
                                    ? QStringLiteral("Download process failed.")
                                    : err);
  }

  clearDownloadState();
}

void ModelManager::updateDownloadProgress() {
  if (!isDownloading())
    return;

  const qint64 currentBytes = localDownloadBytes();
  emit downloadProgress(m_downloadingVariant.id, currentBytes,
                        m_downloadTotalBytes);
}

bool ModelManager::parseRemoteModel(const QJsonObject &object,
                                    RemoteModel &model) const {
  model.id = object.value(QStringLiteral("id")).toString();
  if (model.id.isEmpty())
    return false;

  const QStringList parts = model.id.split(QLatin1Char('/'));
  if (parts.size() == 2) {
    model.author = parts.first();
    model.displayName = parts.last();
  } else {
    model.displayName = model.id;
  }

  model.downloads = object.value(QStringLiteral("downloads")).toInteger();
  model.likes = object.value(QStringLiteral("likes")).toInteger();
  model.lastModified = object.value(QStringLiteral("lastModified")).toString();

  const QJsonArray tagsArray = object.value(QStringLiteral("tags")).toArray();
  for (const QJsonValue &v : tagsArray) {
    model.tags.append(v.toString());
  }

  return true;
}

QList<ModelManager::ModelVariant>
ModelManager::parseVariants(const QString &repoId,
                            const QJsonObject &object) const {
  QList<ModelVariant> variants;
  const QJsonArray siblings =
      object.value(QStringLiteral("siblings")).toArray();

  for (const QJsonValue &v : siblings) {
    if (!v.isObject())
      continue;

    const QString rfilename =
        v.toObject().value(QStringLiteral("rfilename")).toString();
    if (!rfilename.endsWith(QStringLiteral(".gguf"), Qt::CaseInsensitive))
      continue;

    ModelVariant variant;
    variant.repoId = repoId;
    variant.fileNames.append(rfilename);
    variant.quantization = detectQuantization(rfilename);
    variant.id = variantId(repoId, variant.fileNames);
    variant.displayName = rfilename;

    variants.append(variant);
  }

  return variants;
}

void ModelManager::resolveVariantSizes(const QString &repoId) {
  if (!m_remoteVariants.contains(repoId))
    return;

  const auto variants = m_remoteVariants.value(repoId);
  for (const auto &var : variants) {
    for (const QString &fileName : var.fileNames) {
      QUrl url(resolveUrl(repoId, fileName));
      QNetworkRequest req(url);
      QNetworkReply *reply = m_networkManager->head(req);

      m_variantSizeReplies.insert(reply, {repoId, fileName});
      connect(reply, &QNetworkReply::finished, this,
              &ModelManager::onVariantFileHeadFinished);
    }
  }
}

void ModelManager::updateVariantSize(const QString &repoId,
                                     const QString &fileName, qint64 size) {
  if (!m_remoteVariants.contains(repoId))
    return;

  auto &list = m_remoteVariants[repoId];
  for (auto &var : list) {
    if (var.fileNames.contains(fileName)) {
      var.sizeBytes += size;
      var.estimatedVramGb = estimateVramGb(var.sizeBytes);
    }
  }

  emit remoteVariantsChanged(repoId);
}

QString ModelManager::variantDirectory(const ModelVariant &variant) const {
  const QString baseDir = storageDirectory();
  QString sanitizedId = variant.id;
  sanitizedId.replace(QLatin1Char('/'), QLatin1Char('_'));
  sanitizedId.replace(QLatin1Char(':'), QLatin1Char('_'));

  return QDir(baseDir).filePath(sanitizedId);
}

double ModelManager::estimateVramGb(qint64 sizeBytes) {
  return (static_cast<double>(sizeBytes) / (1024.0 * 1024.0 * 1024.0)) * 1.2;
}

QString ModelManager::detectQuantization(const QString &fileName) {
  const QString upper = fileName.toUpper();
  const QStringList quants = {
      QStringLiteral("Q4_K_M"), QStringLiteral("Q4_K_S"),
      QStringLiteral("Q5_K_M"), QStringLiteral("Q5_K_S"),
      QStringLiteral("Q8_0"),   QStringLiteral("Q2_K"),
      QStringLiteral("Q3_K_L"), QStringLiteral("Q3_K_M"),
      QStringLiteral("Q3_K_S"), QStringLiteral("Q6_K"),
      QStringLiteral("FP16"),   QStringLiteral("F16")};

  for (const QString &q : quants) {
    if (upper.contains(q))
      return q;
  }

  return QStringLiteral("Unknown");
}

QString ModelManager::variantId(const QString &repoId,
                                const QStringList &fileNames) {
  if (fileNames.isEmpty())
    return repoId;

  return QStringLiteral("%1:%2").arg(repoId, fileNames.first());
}

QString ModelManager::resolveUrl(const QString &repoId,
                                 const QString &fileName) {
  return QStringLiteral("https://huggingface.co/%1/resolve/main/%2")
      .arg(repoId, fileName);
}

void ModelManager::loadSelectedModel() {
  const QString configPath =
      QDir(storageDirectory()).filePath(QStringLiteral("selected_model.json"));
  QFile file(configPath);
  if (!file.open(QIODevice::ReadOnly))
    return;

  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (doc.isObject()) {
    const QJsonObject obj = doc.object();
    m_selectedModelId = obj.value(QStringLiteral("id")).toString();
    m_selectedModel.id = m_selectedModelId;
    m_selectedModel.repoId = obj.value(QStringLiteral("repoId")).toString();
    m_selectedModel.displayName =
        obj.value(QStringLiteral("displayName")).toString();
  }
}

void ModelManager::saveSelectedModel(const ModelVariant &variant) {
  const QString configPath =
      QDir(storageDirectory()).filePath(QStringLiteral("selected_model.json"));
  QFile file(configPath);
  if (!file.open(QIODevice::WriteOnly))
    return;

  QJsonObject obj;
  obj.insert(QStringLiteral("id"), variant.id);
  obj.insert(QStringLiteral("repoId"), variant.repoId);
  obj.insert(QStringLiteral("displayName"), variant.displayName);

  file.write(QJsonDocument(obj).toJson());
}

void ModelManager::clearDownloadState() {
  if (m_downloadProcess) {
    m_downloadProcess->deleteLater();
    m_downloadProcess = nullptr;
  }

  m_downloadingVariant = ModelVariant{};
  m_downloadFiles.clear();
  m_downloadDirectory.clear();
  m_downloadTotalBytes = 0;
  m_downloadCompletedBytes = 0;
  m_downloadCancelled = false;
}

QString ModelManager::hfExecutable() const {
  return QStandardPaths::findExecutable(QStringLiteral("hf"));
}

qint64 ModelManager::localDownloadBytes() const {
  if (m_downloadDirectory.isEmpty())
    return 0;

  qint64 total = 0;
  QDirIterator it(m_downloadDirectory,
                  QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);

  while (it.hasNext()) {
    it.next();
    total += it.fileInfo().size();
  }

  return total;
}