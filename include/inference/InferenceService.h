#pragma once

#include <QObject>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "LlamaManager.h"
#include "ModelManager.h"
#include "../voice/ITranscriber.h"

class LlmClient;
class TtsManager;
class QNetworkAccessManager;

class InferenceService : public QObject {
    Q_OBJECT

public:
    enum class SttModel {
        Nemotron35,
        NemotronEnglish,
        ParakeetTdt,
        ParakeetCtc
    };

    Q_ENUM(SttModel)

    enum class LlmMode {
        Local,
        Remote
    };

    Q_ENUM(LlmMode)

    enum class LlmAuthType {
        None,
        Bearer
    };

    Q_ENUM(LlmAuthType)

    struct LlmConfig {
        LlmMode mode =
            LlmMode::Local;

        QString endpoint;

        QString model;

        LlmAuthType authType =
            LlmAuthType::None;

        QString apiKey;
    };

public:
    explicit InferenceService(
        QObject *parent = nullptr
    );

    ~InferenceService() override;

    bool initialize(
        LlamaManager::Backend llamaBackend =
            LlamaManager::Backend::Vulkan,
        const QString &sttModelPath =
            QString(),
        SttModel sttModel =
            SttModel::Nemotron35
    );

    bool initialize(
        LlamaManager::Backend llamaBackend,
        const QString &sttModelPath,
        SttModel sttModel,
        const LlmConfig &llmConfig
    );

    bool setLlmConfig(
        const LlmConfig &config
    );

    LlmMode llmMode() const;

    QString llmEndpoint() const;

    QString llmModel() const;

    // -------------------------------------------------------------------------
    // Models
    // -------------------------------------------------------------------------

    ModelManager *models() const;

    QString modelDirectory() const;

    bool setModelDirectory(
        const QString &directory
    );

    void searchLlmModels(
        const QString &query,
        int limit = 30
    );

    void inspectLlmModel(
        const QString &repoId
    );

    QList<ModelManager::RemoteModel>
    remoteLlmModels() const;

    QList<ModelManager::ModelVariant>
    remoteLlmVariants(
        const QString &repoId
    ) const;

    QString selectedLlmModelId() const;

    ModelManager::ModelVariant
    selectedLlmModel() const;

    bool selectLlmModel(
        const ModelManager::ModelVariant &variant
    );

    bool selectLlmModel(
        const QString &modelPathOrId
    );

    void downloadLlmModel(
        const ModelManager::ModelVariant &variant
    );

    void cancelModelDownload();

    // -------------------------------------------------------------------------
    // LLM
    // -------------------------------------------------------------------------

    void sendChatRequest(
        const QJsonArray &messages,
        const QString &model = QString(),
        double temperature = 0.7,
        int timeoutMs = 120000,
        const QString &grammar = QString(),
        const QJsonObject &responseFormat = QJsonObject()
    );

    void abortChatRequest();

    bool isLlmReady() const;

    // -------------------------------------------------------------------------
    // STT
    // -------------------------------------------------------------------------

    QString transcribe(
        const std::vector<float> &pcm32f
    );

    bool isSttReady() const;

    SttModel sttModel() const;

    QString sttModelName() const;

    QString sttModelPath() const;

    bool setSttModel(
        SttModel model
    );

    bool setSttModelName(
        const QString &model
    );

    QStringList availableSttModels() const;

    // -------------------------------------------------------------------------
    // TTS
    // -------------------------------------------------------------------------

    bool isTtsReady() const;

    bool isTtsEnabled() const;

    void setTtsEnabled(
        bool enabled
    );

    void speak(
        const QString &text,
        int speakerId = 0
    );

    void stopSpeech();

    QString ttsVoice() const;

    void setTtsVoice(
        const QString &voice
    );

    QStringList ttsVoices() const;

    void refreshTtsVoices();

    // -------------------------------------------------------------------------
    // OCR
    // -------------------------------------------------------------------------

    QString extractText(
        const QImage &image
    );

signals:
    void llmDelta(
        const QString &text
    );

    void llmFinished();

    void llmError(
        const QString &error
    );

    void llmReady();

    void llmConfigurationChanged();

    void transcriptionFinished(
        const QString &text
    );

    void transcriptionError(
        const QString &error
    );

    void sttModelChanged(
        const QString &model
    );

    void ttsSentenceFinished();

    void ttsError(
        const QString &error
    );

    void ttsEnabledChanged(
        bool enabled
    );

    void ttsVoiceChanged(
        const QString &voice
    );

    void ttsVoicesChanged(
        const QStringList &voices
    );

    void ttsReady();

    void remoteLlmModelsChanged();

    void remoteLlmVariantsChanged(
        const QString &repoId
    );

    void modelDirectoryChanged(
        const QString &directory
    );

    void selectedLlmModelChanged(
        const QString &modelId
    );

    void modelDownloadStarted(
        const QString &modelId
    );

    void modelDownloadProgress(
        const QString &modelId,
        qint64 received,
        qint64 total
    );

    void modelDownloadFinished(
        const QString &modelId
    );

    void modelDownloadError(
        const QString &modelId,
        const QString &error
    );

    void serviceError(
        const QString &error
    );

private slots:
    void onLlmServerReady();

    void onLlamaError(
        const QString &error
    );

    void onTtsServerReady();

    void onModelSelected(
        const QString &modelId
    );

private:
    bool configureRemoteLlm();

    bool validateLlmConfig(
        const LlmConfig &config,
        QString *error = nullptr
    ) const;

    bool startSelectedLlmModel();

    QString resolveSttModelFilename(
        SttModel model
    ) const;

    QString sttModelToString(
        SttModel model
    ) const;

    SttModel sttModelFromString(
        const QString &model
    ) const;

    int resolveSttGpu() const;

    QString resolveSttModelPath(
        const QString &requestedPath
    ) const;

private:
    std::unique_ptr<ModelManager> m_modelManager;

    std::unique_ptr<LlamaManager> m_llamaManager;

    std::unique_ptr<LlmClient> m_llmClient;

    std::unique_ptr<TtsManager> m_ttsManager;

    std::unique_ptr<ITranscriber> m_stt;

    QNetworkAccessManager *m_networkManager =
        nullptr;

    LlamaManager::Backend m_llamaBackend =
        LlamaManager::Backend::Vulkan;

    SttModel m_sttModel =
        SttModel::Nemotron35;

    LlmConfig m_llmConfig;

    QString m_llmEndpoint;

    QString m_llmModel;

    QString m_sttModelPath;

    bool m_initialized =
        false;

    bool m_llmReady =
        false;

    bool m_sttReady =
        false;

    bool m_ttsReady =
        false;
};