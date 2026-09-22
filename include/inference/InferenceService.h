#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <memory>
#include <vector>

#include "../voice/ITranscriber.h"
#include "../voice/TtsManager.h"
#include "LlamaManager.h"
#include "LlmClient.h"
#include "ModelManager.h"
#include "embedding/EmbeddingModel.h"

class LlmClient;
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

  using RequestToken = QUuid;

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

  RequestToken sendChatRequest(const QJsonArray &messages,
                             const QString &model = QString(),
                             double temperature = 0.7, int timeoutMs = 120000,
                             const QString &grammar = QString(),
                             const QJsonObject &responseFormat = QJsonObject(),
                             const QJsonArray &tools = QJsonArray(),
                             const QString &sessionId = QString());

  void abortChatRequest(const RequestToken &token);
  void abortAllChatRequests();

  bool isRequestActive(const RequestToken &token) const;
  bool hasActiveRequests() const;

  bool isLlmReady() const;

  // -----------------------------------------------------------------
  // Speech to text
  // -----------------------------------------------------------------

  QString transcribe(const std::vector<float> &pcm32f);
  bool isSttReady() const;
  SttModel sttModel() const;
  QString sttModelName() const;
  QString sttModelPath() const;
  bool setSttModel(SttModel model);
  bool setSttModelName(const QString &model);
  QStringList availableSttModels() const;

  bool startSttStreaming(int32_t rightContext = 1);
  void feedSttAudio(const std::vector<float> &pcm);
  void stopSttStreaming();
  bool isSttStreaming() const;

  // -----------------------------------------------------------------
  // Text to speech
  // -----------------------------------------------------------------

  bool isTtsReady() const;
  bool isTtsEnabled() const;
  void setTtsEnabled(bool enabled);
  void speak(const QString &text, int speakerId = 0);
  void stopSpeech();
  QString ttsVoice() const;
  void setTtsVoice(const QString &voice);
  QStringList ttsVoices() const;
  void refreshTtsVoices();

  // True while the audio sink is actively playing a chunk. Consumers
  // driving a face can use this to know when the mouth should be
  // running the viseme timeline rather than idling.
  bool isTtsSpeaking() const;

  // -----------------------------------------------------------------
  // Embeddings
  // -----------------------------------------------------------------

  std::vector<float> embed(const QString &text);

  bool isEmbedderReady() const;

  int embedderDimensions() const;

  QString extractText(const QImage &image);

signals:
  void llmDelta(const RequestToken &token, const QString &text);
  void llmFinished(const RequestToken &token);
  void llmToolCalls(const RequestToken &token, const QJsonArray &toolCalls);
  void llmError(const RequestToken &token, const QString &error);

  void llmReady();
  void llmConfigurationChanged();

  void transcriptionFinished(const QString &text);
  void transcriptionError(const QString &error);
  void sttModelChanged(const QString &model);

  void liveSegment(const QString &text, bool isFinal);

  void sttStreamOpened();
  void sttStreamClosed();

  void ttsSentenceFinished();
  void ttsError(const QString &error);
  void ttsEnabledChanged(bool enabled);
  void ttsVoiceChanged(const QString &voice);
  void ttsVoicesChanged(const QStringList &voices);
  void ttsReady();

  // Emitted when a TTS sentence's audio is ready. Carries the audio and
  // the viseme timeline for that sentence. Consumers driving a face
  // should subscribe; consumers that only care about playback can
  // ignore it.
  void ttsChunkReady(const AudioChunk &chunk);

  // Emitted when playback of a chunk begins. Consumers use this to
  // start their viseme clock.
  void ttsChunkPlaybackStarted(const AudioChunk &chunk);

  void embedderReady();
  void embedderError(const QString &error);

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

  void onClientDelta(const LlmClient::Token &token, const QString &text);
  void onClientFinished(const LlmClient::Token &token);
  void onClientToolCalls(const LlmClient::Token &token,
                         const QJsonArray &toolCalls);
  void onClientError(const LlmClient::Token &token, const QString &error);

private:
  bool configureRemoteLlm();
  bool validateLlmConfig(const LlmConfig &config, QString *error = nullptr) const;
  bool startSelectedLlmModel();

  bool startEmbedder();

  QString resolveSttModelFilename(SttModel model) const;
  QString sttModelToString(SttModel model) const;
  SttModel sttModelFromString(const QString &model) const;
  int resolveSttGpu() const;
  QString resolveSttModelPath(const QString &requestedPath) const;

  std::unique_ptr<ModelManager> m_modelManager;
  std::unique_ptr<LlamaManager> m_llamaManager;
  std::unique_ptr<LlmClient> m_llmClient;
  std::unique_ptr<TtsManager> m_ttsManager;
  std::unique_ptr<ITranscriber> m_stt;
  std::unique_ptr<EmbeddingModel> m_embedder;

  QNetworkAccessManager *m_networkManager = nullptr;

  LlamaManager::Backend m_llamaBackend = LlamaManager::Backend::Vulkan;

  SttModel m_sttModel = SttModel::Nemotron35;

  LlmConfig m_llmConfig;

  QString m_llmEndpoint;
  QString m_llmModel;
  QString m_sttModelPath;

  bool m_initialized = false;
  bool m_llmReady = false;
  bool m_sttReady = false;
  bool m_sttStreaming = false;
  bool m_ttsReady = false;
  bool m_ttsSpeaking = false;
  bool m_embedderReady = false;
};