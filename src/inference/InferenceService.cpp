#include "../../include/inference/InferenceService.h"

#include "../include/inference/LlmClient.h"
#include "../include/vision/OcrService.h"
#include "../include/voice/TtsManager.h"
#include "../include/voice/WhisperTranscriber.h"
#include "../include/voice/ParakeetTranscriber.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QDebug>

#include "inference/LlamaManager.h"
#include "inference/ModelManager.h"


InferenceService::InferenceService(
    QObject *parent
)
    : QObject(parent)
      , m_modelManager(
          std::make_unique<ModelManager>(
              this
          )
      )
      , m_llamaManager(
          std::make_unique<LlamaManager>(
              this
          )
      )
      , m_ttsManager(
          std::make_unique<TtsManager>(
              this
          )
      )
      , m_networkManager(
          new QNetworkAccessManager(
              this
          )
      ) {
    m_networkManager->setProxy(
        QNetworkProxy::NoProxy
    );

    m_llmClient =
            std::make_unique<LlmClient>(
                m_networkManager,
                this
            );

    // -------------------------------------------------------------------------
    // LLM client
    // -------------------------------------------------------------------------

    connect(
        m_llmClient.get(),
        &LlmClient::deltaReceived,
        this,
        &InferenceService::llmDelta
    );

    connect(
        m_llmClient.get(),
        &LlmClient::requestFinished,
        this,
        &InferenceService::llmFinished
    );

    connect(
        m_llmClient.get(),
        &LlmClient::requestError,
        this,
        [this](const QString &error) {
            emit llmError(
                error
            );

            emit serviceError(
                error
            );
        }
    );

    // -------------------------------------------------------------------------
    // Llama
    // -------------------------------------------------------------------------

    connect(
        m_llamaManager.get(),
        &LlamaManager::serverReady,
        this,
        &InferenceService::onLlmServerReady
    );

    connect(
        m_llamaManager.get(),
        &LlamaManager::errorOccurred,
        this,
        &InferenceService::onLlamaError
    );

    // -------------------------------------------------------------------------
    // Model manager
    // -------------------------------------------------------------------------

    connect(
        m_modelManager.get(),
        &ModelManager::remoteModelsChanged,
        this,
        &InferenceService::remoteLlmModelsChanged
    );

    connect(
        m_modelManager.get(),
        &ModelManager::remoteVariantsChanged,
        this,
        &InferenceService::remoteLlmVariantsChanged
    );

    connect(
        m_modelManager.get(),
        &ModelManager::storageDirectoryChanged,
        this,
        &InferenceService::modelDirectoryChanged
    );

    connect(
        m_modelManager.get(),
        &ModelManager::selectedModelChanged,
        this,
        &InferenceService::onModelSelected
    );

    connect(
        m_modelManager.get(),
        &ModelManager::downloadStarted,
        this,
        &InferenceService::modelDownloadStarted
    );

    connect(
        m_modelManager.get(),
        &ModelManager::downloadProgress,
        this,
        &InferenceService::modelDownloadProgress
    );

    connect(
        m_modelManager.get(),
        &ModelManager::downloadFinished,
        this,
        &InferenceService::modelDownloadFinished
    );

    connect(
        m_modelManager.get(),
        &ModelManager::downloadError,
        this,
        &InferenceService::modelDownloadError
    );

    // -------------------------------------------------------------------------
    // TTS
    // -------------------------------------------------------------------------

    connect(
        m_ttsManager.get(),
        &TtsManager::serverReady,
        this,
        &InferenceService::onTtsServerReady
    );

    connect(
        m_ttsManager.get(),
        &TtsManager::sentenceFinished,
        this,
        &InferenceService::ttsSentenceFinished
    );

    connect(
        m_ttsManager.get(),
        &TtsManager::errorOccurred,
        this,
        [this](const QString &error) {
            emit ttsError(
                error
            );

            emit serviceError(
                error
            );
        }
    );

    connect(
        m_ttsManager.get(),
        &TtsManager::voiceChanged,
        this,
        &InferenceService::ttsVoiceChanged
    );

    connect(
        m_ttsManager.get(),
        &TtsManager::voicesChanged,
        this,
        &InferenceService::ttsVoicesChanged
    );
}


InferenceService::~InferenceService() = default;


bool InferenceService::initialize(
    LlamaManager::Backend llamaBackend,
    const QString &sttModelPath
) {
    if (m_initialized)
        return true;

    m_llamaBackend =
            llamaBackend;

    qDebug()
            << "[InferenceService] initialize backend="
            << static_cast<int>(
                m_llamaBackend
            );

    // -------------------------------------------------------------------------
    // LLM
    // -------------------------------------------------------------------------

    const QString remoteEndpoint =
            qEnvironmentVariable(
                "TALOS_LLM_URL"
            ).trimmed();

    if (!remoteEndpoint.isEmpty()) {
        m_llmEndpoint =
                remoteEndpoint;

        m_llmReady =
                true;

        emit llmReady();

        qDebug()
                << "[InferenceService] Using remote LLM:"
                << m_llmEndpoint;
    } else {
        if (
            !startSelectedLlmModel()
        ) {
            qWarning()
                    << "[InferenceService]"
                    << "No installed local LLM model is selected.";
        }
    }

    // -------------------------------------------------------------------------
    // TTS
    // -------------------------------------------------------------------------

    if (
        !m_ttsManager->initialize(
            QString(),
            true
        )
    ) {
        emit serviceError(
            QStringLiteral(
                "Failed to initialize TTS."
            )
        );
    }

    // -------------------------------------------------------------------------
    // STT
    // -------------------------------------------------------------------------

    const QString backend =
            qEnvironmentVariable(
                "TALOS_STT_BACKEND",
                "parakeet"
            ).trimmed().toLower();

    const QString resolvedSttPath =
            resolveSttModelPath(
                sttModelPath
            );

    if (resolvedSttPath.isEmpty()) {
        emit serviceError(
            QStringLiteral(
                "No STT model path was found."
            )
        );
    } else {
        if (backend == QStringLiteral("parakeet")) {
            qDebug()
                    << "[InferenceService] Using Parakeet STT:"
                    << resolvedSttPath;

            auto transcriber =
                    std::make_unique<ParakeetTranscriber>(
                        resolvedSttPath,
                        this
                    );

            if (
                transcriber->isLoaded()
            ) {
                m_stt = std::move(
                    transcriber
                );

                m_sttReady =
                        true;
            } else {
                emit serviceError(
                    QStringLiteral(
                        "Failed to load Parakeet STT model: %1"
                    ).arg(
                        resolvedSttPath
                    )
                );
            }
        } else if (
            backend == QStringLiteral("whisper")
        ) {
            qDebug()
                    << "[InferenceService] Using Whisper STT:"
                    << resolvedSttPath;

            auto transcriber =
                    std::make_unique<WhisperTranscriber>(
                        resolvedSttPath,
                        this
                    );

            if (
                transcriber->isLoaded()
            ) {
                m_stt =
                        std::move(
                            transcriber
                        );

                m_sttReady =
                        true;
            } else {
                emit serviceError(
                    QStringLiteral(
                        "Failed to load Whisper STT model: %1"
                    ).arg(
                        resolvedSttPath
                    )
                );
            }
        } else {
            emit serviceError(
                QStringLiteral(
                    "Unknown STT backend '%1'. "
                    "Use 'whisper' or 'parakeet'."
                ).arg(
                    backend
                )
            );
        }
    }

    m_initialized =
            true;

    return true;
}


// -----------------------------------------------------------------------------
// Models
// -----------------------------------------------------------------------------

ModelManager *
InferenceService::models() const {
    return m_modelManager.get();
}


QString InferenceService::modelDirectory() const {
    if (!m_modelManager)
        return QString();

    return m_modelManager
            ->storageDirectory();
}


bool InferenceService::setModelDirectory(
    const QString &directory
) {
    if (!m_modelManager)
        return false;

    return m_modelManager
            ->setStorageDirectory(
                directory
            );
}


void InferenceService::searchLlmModels(
    const QString &query,
    int limit
) {
    if (m_modelManager) {
        m_modelManager
                ->searchRemoteLlmModels(
                    query,
                    limit
                );
    }
}


void InferenceService::inspectLlmModel(
    const QString &repoId
) {
    if (m_modelManager) {
        m_modelManager
                ->inspectRemoteModel(
                    repoId
                );
    }
}


QList<ModelManager::RemoteModel>
InferenceService::remoteLlmModels() const {
    if (!m_modelManager)
        return {};

    return m_modelManager
            ->remoteModels();
}


QList<ModelManager::ModelVariant>
InferenceService::remoteLlmVariants(
    const QString &repoId
) const {
    if (!m_modelManager)
        return {};

    return m_modelManager
            ->remoteVariants(
                repoId
            );
}


QString InferenceService::selectedLlmModelId() const {
    if (!m_modelManager)
        return QString();

    return m_modelManager
            ->selectedModelId();
}


ModelManager::ModelVariant
InferenceService::selectedLlmModel() const {
    if (!m_modelManager)
        return {};

    return m_modelManager
            ->selectedModel();
}


bool InferenceService::selectLlmModel(
    const ModelManager::ModelVariant &variant
) {
    if (!m_modelManager)
        return false;

    return m_modelManager
            ->selectModel(
                variant
            );
}


void InferenceService::downloadLlmModel(
    const ModelManager::ModelVariant &variant
) {
    if (m_modelManager) {
        m_modelManager
                ->downloadModel(
                    variant
                );
    }
}


void InferenceService::cancelModelDownload() {
    if (m_modelManager)
        m_modelManager
                ->cancelDownload();
}


void InferenceService::onModelSelected(
    const QString &modelId
) {
    emit selectedLlmModelChanged(
        modelId
    );

    if (!m_initialized)
        return;

    startSelectedLlmModel();
}


bool InferenceService::startSelectedLlmModel() {
    if (
        !m_modelManager ||
        !m_llamaManager
    ) {
        return false;
    }

    const ModelManager::ModelVariant
            variant =
                    m_modelManager
                    ->selectedModel();

    if (variant.id.isEmpty())
        return false;

    if (
        !m_modelManager
        ->isVariantInstalled(
            variant
        )
    ) {
        qWarning()
                << "[InferenceService] Selected model is not installed:"
                << variant.id;

        return false;
    }

    const QString modelPath =
            m_modelManager
            ->variantEntryPath(
                variant
            );

    if (modelPath.isEmpty()) {
        qWarning()
                << "[InferenceService] Selected model has no local path:"
                << variant.id;

        return false;
    }

    const QFileInfo modelInfo(
        modelPath
    );

    if (
        !modelInfo.exists() ||
        !modelInfo.isFile() ||
        !modelInfo.isReadable()
    ) {
        qWarning()
                << "[InferenceService] Model path is invalid:"
                << modelPath;

        return false;
    }

    /*
     * llama.cpp's OpenAI-compatible /v1/models response
     * identifies the loaded model as:
     *
     * /models/<filename>
     *
     * Keep the request model ID synchronized with that
     * exact container-side path.
     */
    m_llmModel =
            QStringLiteral(
                "/models/%1"
            ).arg(
                modelInfo.fileName()
            );

    qDebug()
            << "[InferenceService] Starting selected model:"
            << variant.id;

    qDebug()
            << "[InferenceService] Host model path:"
            << modelPath;

    qDebug()
            << "[InferenceService] API model ID:"
            << m_llmModel;

    qDebug()
            << "[InferenceService] Llama backend:"
            << static_cast<int>(
                m_llamaBackend
            );

    m_llmReady =
            false;

    m_llamaManager->stop();

    if (
        !m_llamaManager->configure(
            modelPath,
            m_llamaBackend
        )
    ) {
        return false;
    }

    m_llmEndpoint =
            QStringLiteral(
                "http://127.0.0.1:8081/v1/chat/completions"
            );

    m_llamaManager->start();

    return true;
}


// -----------------------------------------------------------------------------
// LLM
// -----------------------------------------------------------------------------

void InferenceService::sendChatRequest(
    const QJsonArray &messages,
    const QString &model,
    double temperature,
    int timeoutMs
) {
    if (!m_llmClient)
        return;

    if (m_llmEndpoint.isEmpty()) {
        emit llmError(
            QStringLiteral(
                "No LLM endpoint is configured."
            )
        );

        return;
    }

    if (!m_llmReady) {
        emit llmError(
            QStringLiteral(
                "LLM service is not ready."
            )
        );

        return;
    }

    LlmClient::Request request;

    request.url =
            m_llmEndpoint;

    request.messages =
            messages;

    request.model =
            model.isEmpty()
                ? m_llmModel
                : model;

    request.temperature =
            temperature;

    request.timeoutMs =
            timeoutMs;

    qDebug()
            << "[InferenceService] Sending chat request"
            << "model=" << request.model
            << "messages=" << request.messages.size()
            << "temperature=" << request.temperature
            << "timeoutMs=" << request.timeoutMs;

    m_llmClient->sendRequest(
        request
    );
}


void InferenceService::abortChatRequest() {
    if (m_llmClient)
        m_llmClient->abortRequest();
}


bool InferenceService::isLlmReady() const {
    return m_llmReady;
}


void InferenceService::onLlmServerReady() {
    m_llmReady =
            true;

    emit llmReady();

    qDebug()
            << "[InferenceService] LLM ready:"
            << m_llmEndpoint
            << "model="
            << m_llmModel;
}


void InferenceService::onLlamaError(
    const QString &error
) {
    m_llmReady =
            false;

    emit llmError(
        error
    );

    emit serviceError(
        error
    );
}


// -----------------------------------------------------------------------------
// STT
// -----------------------------------------------------------------------------

QString InferenceService::transcribe(
    const std::vector<float> &pcm32f
) {
    if (
        !m_sttReady ||
        !m_stt ||
        pcm32f.empty()
    ) {
        return QString();
    }

    return m_stt->transcribe(
        pcm32f
    );
}


bool InferenceService::isSttReady() const {
    return m_sttReady;
}


// -----------------------------------------------------------------------------
// TTS
// -----------------------------------------------------------------------------

bool InferenceService::isTtsReady() const {
    return m_ttsReady;
}


bool InferenceService::isTtsEnabled() const {
    if (!m_ttsManager)
        return false;

    return m_ttsManager
            ->isEnabled();
}


void InferenceService::setTtsEnabled(
    bool enabled
) {
    if (!m_ttsManager)
        return;

    m_ttsManager
            ->setEnabled(
                enabled
            );

    emit ttsEnabledChanged(
        enabled
    );
}


void InferenceService::speak(
    const QString &text,
    int speakerId
) {
    if (
        !m_ttsManager ||
        !m_ttsReady ||
        !m_ttsManager
        ->isEnabled()
    ) {
        return;
    }

    m_ttsManager
            ->enqueueSentence(
                text,
                speakerId
            );
}


void InferenceService::stopSpeech() {
    if (m_ttsManager)
        m_ttsManager
                ->stopAndClear();
}


QString InferenceService::ttsVoice() const {
    if (!m_ttsManager)
        return QString();

    return m_ttsManager
            ->voice();
}


void InferenceService::setTtsVoice(
    const QString &voice
) {
    if (m_ttsManager) {
        m_ttsManager
                ->setVoice(
                    voice
                );
    }
}


QStringList InferenceService::ttsVoices() const {
    if (!m_ttsManager)
        return {};

    return m_ttsManager
            ->availableVoices();
}


void InferenceService::refreshTtsVoices() {
    if (m_ttsManager)
        m_ttsManager
                ->refreshVoices();
}


void InferenceService::onTtsServerReady() {
    m_ttsReady =
            true;

    emit ttsReady();

    qDebug()
            << "[InferenceService] TTS ready.";
}


// -----------------------------------------------------------------------------
// OCR
// -----------------------------------------------------------------------------

QString InferenceService::extractText(
    const QImage &image
) {
    return OcrService::extractText(
        image
    );
}


QString InferenceService::resolveSttModelPath(
    const QString &requestedPath
) const {
    const QString backend =
            qEnvironmentVariable(
                "TALOS_STT_BACKEND",
                "parakeet"
            ).trimmed().toLower();

    // -------------------------------------------------------------------------
    // 1. Explicit environment override
    // -------------------------------------------------------------------------

    const QString environmentPath =
            qEnvironmentVariable(
                "TALOS_STT_MODEL"
            ).trimmed();

    if (!environmentPath.isEmpty()) {
        const QFileInfo info(
            environmentPath
        );

        if (
            info.exists() &&
            info.isFile() &&
            info.isReadable()
        ) {
            qDebug()
                    << "[InferenceService] Using TALOS_STT_MODEL:"
                    << info.absoluteFilePath();

            return info.absoluteFilePath();
        }

        qWarning()
                << "[InferenceService] TALOS_STT_MODEL is invalid:"
                << environmentPath;
    }

    // -------------------------------------------------------------------------
    // 2. Explicit function argument
    //
    // Only use it when it matches the selected backend.
    //
    // Whisper expects:
    //   *.bin
    //
    // Parakeet expects:
    //   *.gguf
    // -------------------------------------------------------------------------

    if (!requestedPath.trimmed().isEmpty()) {
        const QFileInfo info(
            requestedPath
        );

        if (
            info.exists() &&
            info.isFile() &&
            info.isReadable()
        ) {
            const QString suffix =
                    info.suffix().toLower();

            const bool validForWhisper =
                    backend == QStringLiteral("whisper") &&
                    (
                        suffix == QStringLiteral("bin") ||
                        suffix == QStringLiteral("gguf")
                    );

            const bool validForParakeet =
                    backend == QStringLiteral("parakeet") &&
                    suffix == QStringLiteral("gguf");

            if (
                validForWhisper ||
                validForParakeet
            ) {
                qDebug()
                        << "[InferenceService] Using requested STT model:"
                        << info.absoluteFilePath();

                return info.absoluteFilePath();
            }

            qWarning()
                    << "[InferenceService] Ignoring STT model incompatible "
                    "with backend:"
                    << backend
                    << requestedPath;
        }
    }

    // -------------------------------------------------------------------------
    // 3. Backend-specific default
    // -------------------------------------------------------------------------

    QString modelPath;

    if (
        backend == QStringLiteral("parakeet")
    ) {
        modelPath =
                QDir(
                    QCoreApplication::applicationDirPath()
                ).filePath(
                    QStringLiteral(
                        "models/tdt-1.1b-f16.gguf"
                    )
                );
    } else if (
        backend == QStringLiteral("whisper")
    ) {
        modelPath =
                QDir(
                    QCoreApplication::applicationDirPath()
                ).filePath(
                    QStringLiteral(
                        "ggml-tiny.en.bin"
                    )
                );
    } else {
        qWarning()
                << "[InferenceService] Unknown STT backend:"
                << backend;

        return QString();
    }

    // -------------------------------------------------------------------------
    // 4. Validate backend default
    // -------------------------------------------------------------------------

    const QFileInfo info(
        modelPath
    );

    if (
        info.exists() &&
        info.isFile() &&
        info.isReadable()
    ) {
        qDebug()
                << "[InferenceService] Using default"
                << backend
                << "STT model:"
                << info.absoluteFilePath();

        return info.absoluteFilePath();
    }

    qWarning()
            << "[InferenceService] Default STT model not found:"
            << modelPath;

    return QString();
}
