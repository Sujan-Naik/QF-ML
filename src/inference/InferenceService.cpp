#include "../../include/inference/InferenceService.h"

#include "../include/embedding/EmbeddingModel.h"
#include "../include/inference/LlmClient.h"
#include "../include/voice/NemoTranscriber.h"
#include "../include/voice/TtsManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QTimer>
#include <QUrl>

#include "app/QfPaths.h"
#include "inference/LlamaManager.h"
#include "inference/ModelManager.h"

InferenceService::InferenceService(QObject *parent)
    : QObject(parent), m_modelManager(std::make_unique<ModelManager>(this)),
      m_llamaManager(std::make_unique<LlamaManager>(this)),
      m_ttsManager(std::make_unique<TtsManager>(this)),
      m_networkManager(new QNetworkAccessManager(this)) {
  m_modelManager->setStorageDirectory(QFPaths::modelsRoot());

  m_networkManager->setProxy(QNetworkProxy::NoProxy);

  m_llmClient = std::make_unique<LlmClient>(m_networkManager, this);

  connect(m_llmClient.get(), &LlmClient::deltaReceived, this,
          &InferenceService::onClientDelta);

  connect(m_llmClient.get(), &LlmClient::requestFinished, this,
          &InferenceService::onClientFinished);

  connect(m_llmClient.get(), &LlmClient::toolCallsReceived, this,
          &InferenceService::onClientToolCalls);

  connect(m_llmClient.get(), &LlmClient::requestError, this,
          &InferenceService::onClientError);

  connect(m_llamaManager.get(), &LlamaManager::serverReady, this,
          &InferenceService::onLlmServerReady);

  connect(m_llamaManager.get(), &LlamaManager::errorOccurred, this,
          &InferenceService::onLlamaError);

  connect(m_modelManager.get(), &ModelManager::remoteModelsChanged, this,
          &InferenceService::remoteLlmModelsChanged);

  connect(m_modelManager.get(), &ModelManager::remoteVariantsChanged, this,
          &InferenceService::remoteLlmVariantsChanged);

  connect(m_modelManager.get(), &ModelManager::storageDirectoryChanged, this,
          &InferenceService::modelDirectoryChanged);

  connect(m_modelManager.get(), &ModelManager::selectedModelChanged, this,
          &InferenceService::onModelSelected);

  connect(m_modelManager.get(), &ModelManager::downloadStarted, this,
          &InferenceService::modelDownloadStarted);

  connect(m_modelManager.get(), &ModelManager::downloadProgress, this,
          &InferenceService::modelDownloadProgress);

  connect(m_modelManager.get(), &ModelManager::downloadFinished, this,
          &InferenceService::modelDownloadFinished);

  connect(m_modelManager.get(), &ModelManager::downloadError, this,
          &InferenceService::modelDownloadError);

  connect(m_ttsManager.get(), &TtsManager::serverReady, this,
          &InferenceService::onTtsServerReady);

  connect(m_ttsManager.get(), &TtsManager::sentenceFinished, this,
          &InferenceService::ttsSentenceFinished);

  connect(m_ttsManager.get(), &TtsManager::errorOccurred, this,
          [this](const QString &error) {
            emit ttsError(error);
            emit serviceError(error);
          });

  connect(m_ttsManager.get(), &TtsManager::voiceChanged, this,
          &InferenceService::ttsVoiceChanged);

  connect(m_ttsManager.get(), &TtsManager::voicesChanged, this,
          &InferenceService::ttsVoicesChanged);
}

InferenceService::~InferenceService() = default;

bool InferenceService::initialize(LlamaManager::Backend llamaBackend,
                                  const QString &sttModelPath,
                                  SttModel sttModel) {
  LlmConfig localConfig;
  localConfig.mode = LlmMode::Local;
  return initialize(llamaBackend, sttModelPath, sttModel, localConfig);
}

bool InferenceService::initialize(LlamaManager::Backend llamaBackend,
                                  const QString &sttModelPath,
                                  SttModel sttModel,
                                  const LlmConfig &llmConfig) {
  if (m_initialized)
    return true;

  QString configError;

  if (!validateLlmConfig(llmConfig, &configError)) {
    emit serviceError(configError);
    return false;
  }

  m_llamaBackend = llamaBackend;
  m_sttModel = sttModel;
  m_llmConfig = llmConfig;

  qDebug() << "[InferenceService] initialize"
           << "llamaBackend=" << static_cast<int>(m_llamaBackend)
           << "sttModel=" << sttModelToString(m_sttModel)
           << "llmMode=" << static_cast<int>(m_llmConfig.mode);

  if (m_llmConfig.mode == LlmMode::Remote) {
    if (!configureRemoteLlm()) {
      return false;
    }
  } else {
    if (!startSelectedLlmModel()) {
      qWarning() << "[InferenceService]"
                 << "No installed local LLM model is selected.";
    }
  }

  if (!m_ttsManager->initialize(QString(), true)) {
    emit serviceError(QStringLiteral("Failed to initialize TTS."));
  }

  startEmbedder();

  const QString environmentModel = qEnvironmentVariable("QF_STT_MODEL").trimmed();

  if (!environmentModel.isEmpty()) {
    const SttModel selectedFromEnvironment = sttModelFromString(environmentModel);

    if (selectedFromEnvironment != SttModel::Nemotron35 ||
        environmentModel.compare(QStringLiteral("nemotron-3.5"),
                                 Qt::CaseInsensitive) == 0) {
      m_sttModel = selectedFromEnvironment;
    }
  }

  const QString resolvedSttPath = resolveSttModelPath(sttModelPath);

  if (resolvedSttPath.isEmpty()) {
    emit serviceError(
        QStringLiteral("No STT model path was found for model '%1'.")
            .arg(sttModelName()));
  } else {
    m_sttModelPath = resolvedSttPath;

    const int gpu = resolveSttGpu();

    qDebug() << "[InferenceService] Using NeMo-Speech STT:" << m_sttModelPath
             << "model=" << sttModelName() << "gpu=" << gpu;

    const QString language = qEnvironmentVariable("QF_STT_LANGUAGE").trimmed();

    auto transcriber =
        std::make_unique<NemoTranscriber>(resolvedSttPath, gpu, language, this);

    if (transcriber->isLoaded()) {
      connect(transcriber.get(), &NemoTranscriber::transcriptionFinished, this,
              &InferenceService::transcriptionFinished);

      connect(transcriber.get(), &NemoTranscriber::transcriptionError, this,
              [this](const QString &error) {
                emit transcriptionError(error);
                emit serviceError(error);
              });

      connect(transcriber.get(), &NemoTranscriber::liveSegment, this,
              &InferenceService::liveSegment);

      connect(transcriber.get(), &NemoTranscriber::streamOpened, this,
              [this]() {
                m_sttStreaming = true;
                emit sttStreamOpened();
              });

      connect(transcriber.get(), &NemoTranscriber::streamClosed, this,
              [this]() {
                m_sttStreaming = false;
                emit sttStreamClosed();
              });

      m_stt = std::move(transcriber);
      m_sttReady = true;

      emit sttModelChanged(sttModelName());
    } else {
      emit serviceError(
          QStringLiteral("Failed to load NeMo-Speech ASR model: %1")
              .arg(resolvedSttPath));
    }
  }

  m_initialized = true;
  return true;
}

bool InferenceService::startEmbedder() {
  if (m_embedderReady) {
    return true;
  }

  const QString modelPath =
      QDir(QFPaths::modelsRoot())
          .filePath(QStringLiteral("embedding/model_quantized.onnx"));

  const QString vocabPath =
      QDir(QFPaths::modelsRoot())
          .filePath(QStringLiteral("embedding/vocab.txt"));

  if (!QFileInfo::exists(modelPath) || !QFileInfo::exists(vocabPath)) {
    qWarning() << "[InferenceService] Embedding model not present at"
               << modelPath;
    emit embedderError(
        QStringLiteral("Embedding model files are missing."));
    return false;
  }

  auto embedder = std::make_unique<EmbeddingModel>();

  if (!embedder->load(modelPath, vocabPath)) {
    emit embedderError(
        QStringLiteral("Failed to load embedding model."));
    return false;
  }

  m_embedder = std::move(embedder);
  m_embedderReady = true;

  qDebug() << "[InferenceService] Embedder ready, dimensions ="
           << m_embedder->dimensions();

  emit embedderReady();
  return true;
}

std::vector<float> InferenceService::embed(const QString &text) {
  if (!m_embedderReady || !m_embedder) {
    return {};
  }

  return m_embedder->embed(text);
}

bool InferenceService::isEmbedderReady() const { return m_embedderReady; }

int InferenceService::embedderDimensions() const {
  if (!m_embedder) {
    return 0;
  }

  return m_embedder->dimensions();
}

bool InferenceService::validateLlmConfig(const LlmConfig &config,
                                         QString *error) const {
  if (config.mode == LlmMode::Local) {
    return true;
  }

  const QString endpoint = config.endpoint.trimmed();

  if (endpoint.isEmpty()) {
    if (error) {
      *error = QStringLiteral("Remote LLM mode requires an endpoint.");
    }
    return false;
  }

  const QUrl url(endpoint);

  if (!url.isValid() || url.isEmpty() || url.scheme().isEmpty() ||
      url.host().isEmpty()) {
    if (error) {
      *error = QStringLiteral("Remote LLM endpoint is invalid: %1").arg(endpoint);
    }
    return false;
  }

  if (config.model.trimmed().isEmpty()) {
    if (error) {
      *error = QStringLiteral("Remote LLM mode requires a model ID.");
    }
    return false;
  }

  if (config.authType == LlmAuthType::Bearer &&
      config.apiKey.trimmed().isEmpty()) {
    if (error) {
      *error = QStringLiteral(
          "Bearer-authenticated remote LLM mode requires an API key.");
    }
    return false;
  }

  return true;
}

bool InferenceService::configureRemoteLlm() {
  QString validationError;

  if (!validateLlmConfig(m_llmConfig, &validationError)) {
    emit serviceError(validationError);
    return false;
  }

  if (m_llamaManager) {
    m_llamaManager->stop();
  }

  m_llmReady = false;
  m_llmEndpoint = m_llmConfig.endpoint.trimmed();
  m_llmModel = m_llmConfig.model.trimmed();
  m_llmReady = true;

  qDebug() << "[InferenceService] Using remote LLM:" << m_llmEndpoint
           << "model=" << m_llmModel << "auth="
           << (m_llmConfig.authType == LlmAuthType::Bearer &&
               !m_llmConfig.apiKey.isEmpty());

  emit llmConfigurationChanged();
  emit llmReady();
  return true;
}

bool InferenceService::setLlmConfig(const LlmConfig &config) {
  QString validationError;

  if (!validateLlmConfig(config, &validationError)) {
    emit llmError(QUuid(), validationError);
    return false;
  }

  abortAllChatRequests();

  m_llmConfig = config;

  emit llmConfigurationChanged();

  if (!m_initialized)
    return true;

  if (m_llmConfig.mode == LlmMode::Remote)
    return configureRemoteLlm();

  m_llmReady = false;
  m_llmEndpoint.clear();
  m_llmModel.clear();

  if (!startSelectedLlmModel()) {
    emit llmError(QUuid(),
                  QStringLiteral("Unable to start the selected local LLM model."));
    return false;
  }

  return true;
}

InferenceService::LlmMode InferenceService::llmMode() const {
  return m_llmConfig.mode;
}

QString InferenceService::llmEndpoint() const { return m_llmEndpoint; }
QString InferenceService::llmModel() const { return m_llmModel; }
ModelManager *InferenceService::models() const { return m_modelManager.get(); }

QString InferenceService::modelDirectory() const {
  if (!m_modelManager)
    return QString();

  return m_modelManager->storageDirectory();
}

bool InferenceService::setModelDirectory(const QString &directory) {
  if (!m_modelManager)
    return false;

  return m_modelManager->setStorageDirectory(directory);
}

void InferenceService::searchLlmModels(const QString &query, int limit) {
  if (m_modelManager) {
    m_modelManager->searchRemoteLlmModels(query, limit);
  }
}

void InferenceService::inspectLlmModel(const QString &repoId) {
  if (m_modelManager) {
    m_modelManager->inspectRemoteModel(repoId);
  }
}

QList<ModelManager::RemoteModel> InferenceService::remoteLlmModels() const {
  if (!m_modelManager)
    return {};

  return m_modelManager->remoteModels();
}

QList<ModelManager::ModelVariant>
InferenceService::remoteLlmVariants(const QString &repoId) const {
  if (!m_modelManager)
    return {};

  return m_modelManager->remoteVariants(repoId);
}

QString InferenceService::selectedLlmModelId() const {
  if (!m_modelManager)
    return QString();

  return m_modelManager->selectedModelId();
}

ModelManager::ModelVariant InferenceService::selectedLlmModel() const {
  if (!m_modelManager)
    return {};

  return m_modelManager->selectedModel();
}

bool InferenceService::selectLlmModel(
    const ModelManager::ModelVariant &variant) {
  if (!m_modelManager)
    return false;

  return m_modelManager->selectModel(variant);
}

bool InferenceService::selectLlmModel(const QString &modelPathOrId) {
  if (!m_modelManager)
    return false;

  return m_modelManager->selectModel(modelPathOrId);
}

void InferenceService::downloadLlmModel(
    const ModelManager::ModelVariant &variant) {
  if (m_modelManager) {
    m_modelManager->downloadModel(variant);
  }
}

void InferenceService::cancelModelDownload() {
  if (m_modelManager)
    m_modelManager->cancelDownload();
}

void InferenceService::onModelSelected(const QString &modelId) {
  emit selectedLlmModelChanged(modelId);

  if (!m_initialized)
    return;

  if (m_llmConfig.mode != LlmMode::Local) {
    return;
  }

  startSelectedLlmModel();
}

bool InferenceService::startSelectedLlmModel() {
  if (m_llmConfig.mode != LlmMode::Local) {
    return false;
  }

  if (!m_modelManager || !m_llamaManager) {
    return false;
  }

  const QString selectedId = m_modelManager->selectedModelId();

  if (selectedId.isEmpty())
    return false;

  QString modelPath;

  QFileInfo directInfo(selectedId);

  if (!directInfo.isAbsolute()) {
    directInfo.setFile(QDir(QFPaths::llmModelsDir()).filePath(selectedId));
  }

  if (directInfo.exists() && directInfo.isFile() && directInfo.isReadable()) {
    modelPath = directInfo.absoluteFilePath();
  } else {
    const ModelManager::ModelVariant variant = m_modelManager->selectedModel();

    if (!variant.id.isEmpty() && m_modelManager->isVariantInstalled(variant)) {
      modelPath = m_modelManager->variantEntryPath(variant);
    }
  }

  if (modelPath.isEmpty()) {
    qWarning() << "[InferenceService] Selected model has no valid local path:"
               << selectedId;
    return false;
  }

  const QFileInfo modelInfo(modelPath);

  if (!modelInfo.exists() || !modelInfo.isFile() || !modelInfo.isReadable()) {
    qWarning() << "[InferenceService] Model path is invalid:" << modelPath;
    return false;
  }

  m_llmModel = QStringLiteral("/models/%1").arg(modelInfo.fileName());

  qDebug() << "[InferenceService] Starting selected local model:" << selectedId;
  qDebug() << "[InferenceService] Host model path:" << modelPath;
  qDebug() << "[InferenceService] API model ID:" << m_llmModel;
  qDebug() << "[InferenceService] Llama backend:"
           << static_cast<int>(m_llamaBackend);

  m_llmReady = false;
  m_llamaManager->stop();

  if (!m_llamaManager->configure(modelPath, m_llamaBackend)) {
    return false;
  }

  m_llmEndpoint = QStringLiteral("http://127.0.0.1:8081/v1/chat/completions");

  m_llamaManager->start();
  return true;
}

InferenceService::RequestToken InferenceService::sendChatRequest(
    const QJsonArray &messages, const QString &model, double temperature,
    int timeoutMs, const QString &grammar, const QJsonObject &responseFormat,
    const QJsonArray &tools, const QString &sessionId) {
  if (!m_llmClient) {
    const RequestToken token = QUuid::createUuid();
    emit llmError(token, QStringLiteral("LLM client is unavailable."));
    return token;
  }

  if (m_llmEndpoint.isEmpty()) {
    const RequestToken token = QUuid::createUuid();
    emit llmError(token, QStringLiteral("No LLM endpoint is configured."));
    return token;
  }

  if (!m_llmReady) {
    const RequestToken token = QUuid::createUuid();
    emit llmError(token, QStringLiteral("LLM service is not ready."));
    return token;
  }

  LlmClient::Request request;

  request.url = m_llmEndpoint;
  request.model = model.isEmpty() ? m_llmModel : model;
  request.messages = messages;
  request.temperature = temperature;
  request.timeoutMs = timeoutMs;
  request.sessionId = sessionId;

  if (m_llmConfig.mode == LlmMode::Local)
    request.grammar = grammar;

  if (m_llmConfig.mode == LlmMode::Remote)
    request.responseFormat = responseFormat;

  request.tools = tools;

  if (m_llmConfig.mode == LlmMode::Remote) {
    request.authType = m_llmConfig.authType == LlmAuthType::Bearer
                           ? LlmClient::AuthType::Bearer
                           : LlmClient::AuthType::None;
    request.apiKey = m_llmConfig.apiKey;
  }

  qDebug() << "[InferenceService] Dispatching chat request"
           << "mode="
           << (m_llmConfig.mode == LlmMode::Local ? QStringLiteral("local")
                                                  : QStringLiteral("remote"))
           << "model=" << request.model
           << "messages=" << request.messages.size()
           << "toolCount=" << request.tools.size()
           << "sessionId=" << request.sessionId;

  return m_llmClient->sendRequest(request);
}

void InferenceService::abortChatRequest(const RequestToken &token) {
  if (m_llmClient)
    m_llmClient->abortRequest(token);
}

void InferenceService::abortAllChatRequests() {
  if (m_llmClient)
    m_llmClient->abortAllRequests();
}

bool InferenceService::isRequestActive(const RequestToken &token) const {
  return m_llmClient && m_llmClient->isActive(token);
}

bool InferenceService::hasActiveRequests() const {
  return m_llmClient && m_llmClient->hasActiveRequests();
}

bool InferenceService::isLlmReady() const { return m_llmReady; }

void InferenceService::onClientDelta(const LlmClient::Token &token,
                                     const QString &text) {
  emit llmDelta(token, text);
}

void InferenceService::onClientFinished(const LlmClient::Token &token) {
  emit llmFinished(token);
}

void InferenceService::onClientToolCalls(const LlmClient::Token &token,
                                         const QJsonArray &toolCalls) {
  emit llmToolCalls(token, toolCalls);
}

void InferenceService::onClientError(const LlmClient::Token &token,
                                     const QString &error) {
  emit llmError(token, error);
  emit serviceError(error);
}

void InferenceService::onLlmServerReady() {
  if (m_llmConfig.mode != LlmMode::Local) {
    return;
  }

  m_llmReady = true;

  emit llmReady();

  qDebug() << "[InferenceService] Local LLM ready:" << m_llmEndpoint
           << "model=" << m_llmModel;
}

void InferenceService::onLlamaError(const QString &error) {
  if (m_llmConfig.mode != LlmMode::Local)
    return;

  m_llmReady = false;

  emit llmError(QUuid(), error);
  emit serviceError(error);
}

QString InferenceService::transcribe(const std::vector<float> &pcm32f) {
  if (!m_sttReady || !m_stt || pcm32f.empty()) {
    return QString();
  }

  return m_stt->transcribe(pcm32f);
}

bool InferenceService::isSttReady() const { return m_sttReady; }
InferenceService::SttModel InferenceService::sttModel() const { return m_sttModel; }
QString InferenceService::sttModelName() const { return sttModelToString(m_sttModel); }
QString InferenceService::sttModelPath() const { return m_sttModelPath; }

bool InferenceService::setSttModel(SttModel model) {
  if (m_sttModel == model)
    return true;

  m_sttModel = model;
  m_sttReady = false;
  m_sttStreaming = false;
  m_stt.reset();

  const QString path = resolveSttModelPath(QString());

  if (path.isEmpty()) {
    emit serviceError(
        QStringLiteral("STT model is not installed: %1").arg(sttModelName()));
    return false;
  }

  const QString language = qEnvironmentVariable("QF_STT_LANGUAGE").trimmed();
  const int gpu = resolveSttGpu();

  auto transcriber =
      std::make_unique<NemoTranscriber>(path, gpu, language, this);

  if (!transcriber->isLoaded()) {
    emit serviceError(QStringLiteral("Failed to load STT model: %1").arg(path));
    return false;
  }

  connect(transcriber.get(), &NemoTranscriber::transcriptionFinished, this,
          &InferenceService::transcriptionFinished);

  connect(transcriber.get(), &NemoTranscriber::transcriptionError, this,
          [this](const QString &error) {
            emit transcriptionError(error);
            emit serviceError(error);
          });

  connect(transcriber.get(), &NemoTranscriber::liveSegment, this,
          &InferenceService::liveSegment);

  connect(transcriber.get(), &NemoTranscriber::streamOpened, this,
          [this]() {
            m_sttStreaming = true;
            emit sttStreamOpened();
          });

  connect(transcriber.get(), &NemoTranscriber::streamClosed, this,
          [this]() {
            m_sttStreaming = false;
            emit sttStreamClosed();
          });

  m_stt = std::move(transcriber);
  m_sttModelPath = path;
  m_sttReady = true;

  emit sttModelChanged(sttModelName());
  return true;
}

bool InferenceService::setSttModelName(const QString &model) {
  const QString normalized = model.trimmed().toLower();

  if (normalized.isEmpty())
    return false;

  const SttModel selected = sttModelFromString(normalized);

  if (normalized != QStringLiteral("nemotron-3.5") &&
      normalized != QStringLiteral("nemotron-en") &&
      normalized != QStringLiteral("parakeet-tdt") &&
      normalized != QStringLiteral("parakeet-ctc")) {
    emit serviceError(QStringLiteral("Unknown STT model '%1'.").arg(model));
    return false;
  }

  return setSttModel(selected);
}

QStringList InferenceService::availableSttModels() const {
  return {QStringLiteral("nemotron-3.5"), QStringLiteral("nemotron-en"),
          QStringLiteral("parakeet-tdt"), QStringLiteral("parakeet-ctc")};
}

bool InferenceService::startSttStreaming(int32_t rightContext) {
  auto *nemo = dynamic_cast<NemoTranscriber *>(m_stt.get());
  if (!nemo) {
    emit serviceError(QStringLiteral("STT streaming requires NeMo-Speech."));
    return false;
  }
  return nemo->startStreaming(rightContext);
}

void InferenceService::feedSttAudio(const std::vector<float> &pcm) {
  auto *nemo = dynamic_cast<NemoTranscriber *>(m_stt.get());
  if (nemo) nemo->feedAudio(pcm);
}

void InferenceService::stopSttStreaming() {
  auto *nemo = dynamic_cast<NemoTranscriber *>(m_stt.get());
  if (nemo) nemo->stopStreaming();
}

bool InferenceService::isSttStreaming() const { return m_sttStreaming; }

bool InferenceService::isTtsReady() const { return m_ttsReady; }

bool InferenceService::isTtsEnabled() const {
  if (!m_ttsManager)
    return false;

  return m_ttsManager->isEnabled();
}

void InferenceService::setTtsEnabled(bool enabled) {
  if (!m_ttsManager)
    return;

  m_ttsManager->setEnabled(enabled);
  emit ttsEnabledChanged(enabled);
}

void InferenceService::speak(const QString &text, int speakerId) {
  if (!m_ttsManager || !m_ttsReady || !m_ttsManager->isEnabled()) {
    return;
  }

  m_ttsManager->enqueueSentence(text, speakerId);
}

void InferenceService::stopSpeech() {
  if (m_ttsManager)
    m_ttsManager->stopAndClear();
}

QString InferenceService::ttsVoice() const {
  if (!m_ttsManager)
    return QString();

  return m_ttsManager->voice();
}

void InferenceService::setTtsVoice(const QString &voice) {
  if (m_ttsManager) {
    m_ttsManager->setVoice(voice);
  }
}

QStringList InferenceService::ttsVoices() const {
  if (!m_ttsManager)
    return {};

  return m_ttsManager->availableVoices();
}

void InferenceService::refreshTtsVoices() {
  if (m_ttsManager)
    m_ttsManager->refreshVoices();
}

void InferenceService::onTtsServerReady() {
  m_ttsReady = true;
  emit ttsReady();
  qDebug() << "[InferenceService] TTS ready.";
}

QString InferenceService::resolveSttModelFilename(SttModel model) const {
  switch (model) {
  case SttModel::Nemotron35:
    return QStringLiteral("nemotron-3.5-asr-streaming-0.6b.q8_0.gguf");

  case SttModel::NemotronEnglish:
    return QStringLiteral("nemotron-speech-streaming-en-0.6b.q8_0.gguf");

  case SttModel::ParakeetTdt:
    return QStringLiteral("parakeet-tdt-0.6b-v3.q8_0.gguf");

  case SttModel::ParakeetCtc:
    return QStringLiteral("parakeet-ctc-1.1b.q8_0.gguf");
  }

  return QString();
}

QString InferenceService::sttModelToString(SttModel model) const {
  switch (model) {
  case SttModel::Nemotron35:
    return QStringLiteral("nemotron-3.5");

  case SttModel::NemotronEnglish:
    return QStringLiteral("nemotron-en");

  case SttModel::ParakeetTdt:
    return QStringLiteral("parakeet-tdt");

  case SttModel::ParakeetCtc:
    return QStringLiteral("parakeet-ctc");
  }

  return QStringLiteral("nemotron-3.5");
}

InferenceService::SttModel
InferenceService::sttModelFromString(const QString &model) const {
  const QString normalized = model.trimmed().toLower();

  if (normalized == QStringLiteral("nemotron-en")) {
    return SttModel::NemotronEnglish;
  }

  if (normalized == QStringLiteral("parakeet-tdt")) {
    return SttModel::ParakeetTdt;
  }

  if (normalized == QStringLiteral("parakeet-ctc")) {
    return SttModel::ParakeetCtc;
  }

  return SttModel::Nemotron35;
}

int InferenceService::resolveSttGpu() const {
  const QString value = qEnvironmentVariable("QF_STT_GPU", "0").trimmed();

  bool ok = false;
  const int gpu = value.toInt(&ok);

  if (!ok)
    return 0;

  return gpu;
}

QString
InferenceService::resolveSttModelPath(const QString &requestedPath) const {
  if (!requestedPath.trimmed().isEmpty()) {
    const QFileInfo info(requestedPath);

    if (info.exists() && info.isFile() && info.isReadable()) {
      qDebug() << "[InferenceService] Using requested STT model:"
               << info.absoluteFilePath();

      return info.absoluteFilePath();
    }

    qWarning() << "[InferenceService] Requested STT model is invalid:"
               << requestedPath;
  }

  const QString environmentPath =
      qEnvironmentVariable("QF_STT_MODEL_PATH").trimmed();

  if (!environmentPath.isEmpty()) {
    const QFileInfo info(environmentPath);

    if (info.exists() && info.isFile() && info.isReadable()) {
      qDebug() << "[InferenceService] Using QF_STT_MODEL_PATH:"
               << info.absoluteFilePath();

      return info.absoluteFilePath();
    }

    qWarning() << "[InferenceService] QF_STT_MODEL_PATH is invalid:"
               << environmentPath;
  }

  const QString filename = resolveSttModelFilename(m_sttModel);

  if (filename.isEmpty())
    return QString();

  const QString modelPath = QDir(QFPaths::sttModelsDir()).filePath(filename);

  const QFileInfo info(modelPath);

  if (info.exists() && info.isFile() && info.isReadable()) {
    qDebug() << "[InferenceService] Using shared STT model:" << modelPath;

    return info.absoluteFilePath();
  }

  qWarning() << "[InferenceService] STT model not found:" << modelPath;

  return QString();
}

QString InferenceService::extractText(const QImage &image) {
  Q_UNUSED(image)
  return {};
}