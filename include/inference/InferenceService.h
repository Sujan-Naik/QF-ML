#pragma once

#include <QObject>
#include <QImage>
#include <QJsonArray>
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
class WhisperTranscriber;


class InferenceService : public QObject {
    Q_OBJECT

public:
    explicit InferenceService(
        QObject *parent = nullptr
    );

    ~InferenceService() override;

    bool initialize(
        LlamaManager::Backend llamaBackend =
                LlamaManager::Backend::Vulkan,
        const QString &sttModelPath =
                QString()
    );

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
        int timeoutMs = 120000
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

    void transcriptionFinished(
        const QString &text
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
    bool startSelectedLlmModel();

    QString resolveSttModelPath(
        const QString &requestedPath
    ) const;

private:
    std::unique_ptr<ModelManager>
    m_modelManager;

    std::unique_ptr<LlamaManager>
    m_llamaManager;

    std::unique_ptr<LlmClient>
    m_llmClient;

    std::unique_ptr<TtsManager>
    m_ttsManager;

    std::unique_ptr<ITranscriber>
    m_stt;

    QNetworkAccessManager *
            m_networkManager = nullptr;

    QString m_llmEndpoint;

    /*
     * This is populated from the actual container model path.
     * llama.cpp advertises this exact value through /v1/models.
     */
    QString m_llmModel;

    bool m_initialized = false;

    bool m_llmReady = false;
    bool m_ttsReady = false;
    bool m_sttReady = false;

    LlamaManager::Backend
    m_llamaBackend =
            LlamaManager::Backend::Vulkan;
};
