#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <memory>
#include <vector>

#include "../voice/ITranscriber.h"
#include "LlamaManager.h"
#include "ModelManager.h"

class LlmClient;
class TtsManager;
class QNetworkAccessManager;

class InferenceService : public QObject {
  Q_OBJECT

public:
  enum class SttModel { Nemotron35, NemotronEnglish, ParakeetTdt, ParakeetCtc };
  Q_ENUM(SttModel)

  enum class LlmMode { Local, Remote };
  Q_ENUM(LlmMode)

  enum class LlmAuthType { None, Bearer };
  Q_ENUM(LlmAuthType)

  // Opaque handle for a single in-flight chat request. Pass it back to
  // abortChatRequest() and compare it against the token carried by every
  // llm* signal to ignore other consumers' streams.
  using RequestToken = QUuid;

  // What to do when a new request is issued while another one is active.
  enum class RequestPolicy {
    // Abort the active request and start the new one. Default.
    Abort,
    // Queue the new request behind the active one. The new request is
    // issued automatically when the active one finishes, errors, or is
    // aborted. Use this for non-critical probes such as the settings
    // test.
    Queue,
  };
  Q_ENUM(RequestPolicy)

  struct LlmConfig {
    LlmMode mode = LlmMode::Local;
    QString endpoint;
    QString model;
    LlmAuthType authType = LlmAuthType::None;
    QString apiKey;
  };

public:
  explicit InferenceService(QObject *parent = nullptr);
  ~InferenceService() override;

  bool initialize(LlamaManager::Backend llamaBackend = LlamaManager::Backend::Vulkan,
                  const QString &sttModelPath = QString(),
                  SttModel sttModel = SttModel::Nemotron35);

  bool initialize(LlamaManager::Backend llamaBackend,
                  const QString &sttModelPath, SttModel sttModel,
                  const LlmConfig &llmConfig);

  bool setLlmConfig(const LlmConfig &config);

  LlmMode llmMode() const;
  QString llmEndpoint() const;
  QString llmModel() const;

  // -------------------------------------------------------------------------
  // Models
  // -------------------------------------------------------------------------

  ModelManager *models() const;
  QString modelDirectory() const;
  bool setModelDirectory(const QString &directory);

  void searchLlmModels(const QString &query, int limit = 30);
  void inspectLlmModel(const QString &repoId);

  QList<ModelManager::RemoteModel> remoteLlmModels() const;
  QList<ModelManager::ModelVariant> remoteLlmVariants(const QString &repoId) const;

  QString selectedLlmModelId() const;
  ModelManager::ModelVariant selectedLlmModel() const;

  bool selectLlmModel(const ModelManager::ModelVariant &variant);
  bool selectLlmModel(const QString &modelPathOrId);

  void downloadLlmModel(const ModelManager::ModelVariant &variant);
  void cancelModelDownload();

  // -------------------------------------------------------------------------
  // LLM
  // -------------------------------------------------------------------------

  // Issues a chat request. Returns a token. Every llm* signal carries
  // the same token for the lifetime of the request. If another request
  // is already active, `policy` decides whether to abort it or queue
  // behind it.
  RequestToken sendChatRequest(const QJsonArray &messages,
                               const QString &model = QString(),
                               double temperature = 0.7, int timeoutMs = 120000,
                               const QString &grammar = QString(),
                               const QJsonObject &responseFormat = QJsonObject(),
                               const QJsonArray &tools = QJsonArray(),
                               RequestPolicy policy = RequestPolicy::Abort);

  // Aborts the request associated with `token`. No-op if the token is
  // stale or refers to a queued request that has not been dispatched.
  void abortChatRequest(const RequestToken &token);

  // Aborts whatever request is currently active, if any.
  void abortActiveChatRequest();

  bool isRequestActive(const RequestToken &token) const;

  RequestToken activeRequestToken() const { return m_activeToken; }

  bool isLlmReady() const;

  // -------------------------------------------------------------------------
  // STT
  // -------------------------------------------------------------------------

  QString transcribe(const std::vector<float> &pcm32f);
  bool isSttReady() const;
  SttModel sttModel() const;
  QString sttModelName() const;
  QString sttModelPath() const;
  bool setSttModel(SttModel model);
  bool setSttModelName(const QString &model);
  QStringList availableSttModels() const;

  // -------------------------------------------------------------------------
  // TTS
  // -------------------------------------------------------------------------

  bool isTtsReady() const;
  bool isTtsEnabled() const;
  void setTtsEnabled(bool enabled);
  void speak(const QString &text, int speakerId = 0);
  void stopSpeech();
  QString ttsVoice() const;
  void setTtsVoice(const QString &voice);
  QStringList ttsVoices() const;
  void refreshTtsVoices();

  // -------------------------------------------------------------------------
  // OCR
  // -------------------------------------------------------------------------

  QString extractText(const QImage &image);

signals:
  // Every signal carries the token returned by sendChatRequest. Consumers
  // must compare it against their own token and ignore anything else.
  void llmDelta(const RequestToken &token, const QString &text);

  void llmFinished(const RequestToken &token);

  // Emitted instead of llmFinished when the response contained
  // tool_calls.
  void llmToolCalls(const RequestToken &token, const QJsonArray &toolCalls);

  void llmError(const RequestToken &token, const QString &error);

  // Emitted whenever the active request changes: a new one starts, one
  // finishes, one errors, or one is aborted. Consumers that want to know
  // "is anyone streaming" can watch this.
  void activeRequestChanged(const RequestToken &token);

  void llmReady();
  void llmConfigurationChanged();

  void transcriptionFinished(const QString &text);
  void transcriptionError(const QString &error);
  void sttModelChanged(const QString &model);

  void ttsSentenceFinished();
  void ttsError(const QString &error);
  void ttsEnabledChanged(bool enabled);
  void ttsVoiceChanged(const QString &voice);
  void ttsVoicesChanged(const QStringList &voices);
  void ttsReady();

  void remoteLlmModelsChanged();
  void remoteLlmVariantsChanged(const QString &repoId);
  void modelDirectoryChanged(const QString &directory);
  void selectedLlmModelChanged(const QString &modelId);

  void modelDownloadStarted(const QString &modelId);
  void modelDownloadProgress(const QString &modelId, qint64 received, qint64 total);
  void modelDownloadFinished(const QString &modelId);
  void modelDownloadError(const QString &modelId, const QString &error);

  void serviceError(const QString &error);

private slots:
  void onLlmServerReady();
  void onLlamaError(const QString &error);
  void onTtsServerReady();
  void onModelSelected(const QString &modelId);

  // LlmClient relay slots. They attach the active token to each signal
  // before re-emitting.
  void onClientDelta(const QString &text);
  void onClientFinished();
  void onClientToolCalls(const QJsonArray &toolCalls);
  void onClientError(const QString &error);

private:
  struct PendingRequest {
    RequestToken token;
    QJsonArray messages;
    QString model;
    double temperature = 0.7;
    int timeoutMs = 120000;
    QString grammar;
    QJsonObject responseFormat;
    QJsonArray tools;
  };

  bool configureRemoteLlm();
  bool validateLlmConfig(const LlmConfig &config, QString *error = nullptr) const;
  bool startSelectedLlmModel();

  QString resolveSttModelFilename(SttModel model) const;
  QString sttModelToString(SttModel model) const;
  SttModel sttModelFromString(const QString &model) const;
  int resolveSttGpu() const;
  QString resolveSttModelPath(const QString &requestedPath) const;

  // Builds the LlmClient::Request from a PendingRequest and hands it to
  // the client. Sets m_activeToken and emits activeRequestChanged.
  void dispatchPendingRequest(const PendingRequest &pending);

  // Pumps the queue if no request is currently active.
  void pumpRequestQueue();

private:
  std::unique_ptr<ModelManager> m_modelManager;
  std::unique_ptr<LlamaManager> m_llamaManager;
  std::unique_ptr<LlmClient> m_llmClient;
  std::unique_ptr<TtsManager> m_ttsManager;
  std::unique_ptr<ITranscriber> m_stt;

  QNetworkAccessManager *m_networkManager = nullptr;

  LlamaManager::Backend m_llamaBackend = LlamaManager::Backend::Vulkan;

  SttModel m_sttModel = SttModel::Nemotron35;

  LlmConfig m_llmConfig;

  QString m_llmEndpoint;
  QString m_llmModel;
  QString m_sttModelPath;

  // The token of the currently-active request, or a null QUuid if none.
  RequestToken m_activeToken;

  // Requests waiting to be sent after the active one finishes. Only
  // populated when a caller uses RequestPolicy::Queue.
  QQueue<PendingRequest> m_requestQueue;

  bool m_initialized = false;
  bool m_llmReady = false;
  bool m_sttReady = false;
  bool m_ttsReady = false;
};