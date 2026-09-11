#include "../../include/inference/InferenceService.h"

#include "../include/inference/LlmClient.h"
#include "../include/voice/TtsManager.h"
#include "../include/voice/NemoTranscriber.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QUrl>
#include <QDebug>

#include "app/QfPaths.h"
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
    )
{
    m_modelManager->setStorageDirectory(
        QFPaths::modelsRoot()
    );

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


// =============================================================================
// Initialization
// =============================================================================

bool InferenceService::initialize(
    LlamaManager::Backend llamaBackend,
    const QString &sttModelPath,
    SttModel sttModel
)
{
    LlmConfig localConfig;

    localConfig.mode =
        LlmMode::Local;

    return initialize(
        llamaBackend,
        sttModelPath,
        sttModel,
        localConfig
    );
}


bool InferenceService::initialize(
    LlamaManager::Backend llamaBackend,
    const QString &sttModelPath,
    SttModel sttModel,
    const LlmConfig &llmConfig
)
{
    if (m_initialized)
        return true;

    QString configError;

    if (
        !validateLlmConfig(
            llmConfig,
            &configError
        )
    ) {

        emit serviceError(
            configError
        );

        return false;
    }

    m_llamaBackend =
        llamaBackend;

    m_sttModel =
        sttModel;

    m_llmConfig =
        llmConfig;

    qDebug()
        << "[InferenceService] initialize"
        << "llamaBackend="
        << static_cast<int>(
            m_llamaBackend
        )
        << "sttModel="
        << sttModelToString(
            m_sttModel
        )
        << "llmMode="
        << static_cast<int>(
            m_llmConfig.mode
        );

    // -------------------------------------------------------------------------
    // LLM
    // -------------------------------------------------------------------------

    if (
        m_llmConfig.mode ==
        LlmMode::Remote
    ) {

        if (!configureRemoteLlm()) {

            return false;
        }

    } else {

        if (!startSelectedLlmModel()) {

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

    const QString environmentModel =
        qEnvironmentVariable(
            "QF_STT_MODEL"
        ).trimmed();

    if (!environmentModel.isEmpty()) {

        const SttModel selectedFromEnvironment =
            sttModelFromString(
                environmentModel
            );

        if (
            selectedFromEnvironment !=
                SttModel::Nemotron35 ||
            environmentModel.compare(
                QStringLiteral(
                    "nemotron-3.5"
                ),
                Qt::CaseInsensitive
            ) == 0
        ) {

            m_sttModel =
                selectedFromEnvironment;
        }
    }

    const QString resolvedSttPath =
        resolveSttModelPath(
            sttModelPath
        );

    if (resolvedSttPath.isEmpty()) {

        emit serviceError(
            QStringLiteral(
                "No STT model path was found for model '%1'."
            ).arg(
                sttModelName()
            )
        );

    } else {

        m_sttModelPath =
            resolvedSttPath;

        const int gpu =
            resolveSttGpu();

        qDebug()
            << "[InferenceService] Using NeMo-Speech STT:"
            << m_sttModelPath
            << "model="
            << sttModelName()
            << "gpu="
            << gpu;

        const QString language =
            qEnvironmentVariable(
                "QF_STT_LANGUAGE"
            ).trimmed();

        auto transcriber =
            std::make_unique<NemoTranscriber>(
                resolvedSttPath,
                gpu,
                language,
                this
            );

        if (transcriber->isLoaded()) {

            connect(
                transcriber.get(),
                &NemoTranscriber::transcriptionFinished,
                this,
                &InferenceService::transcriptionFinished
            );

            connect(
                transcriber.get(),
                &NemoTranscriber::transcriptionError,
                this,
                [this](const QString &error) {

                    emit transcriptionError(
                        error
                    );

                    emit serviceError(
                        error
                    );
                }
            );

            m_stt =
                std::move(
                    transcriber
                );

            m_sttReady =
                true;

            emit sttModelChanged(
                sttModelName()
            );

        } else {

            emit serviceError(
                QStringLiteral(
                    "Failed to load NeMo-Speech ASR model: %1"
                ).arg(
                    resolvedSttPath
                )
            );
        }
    }

    m_initialized =
        true;

    return true;
}


bool InferenceService::validateLlmConfig(
    const LlmConfig &config,
    QString *error
) const
{
    if (
        config.mode ==
        LlmMode::Local
    ) {
        return true;
    }

    const QString endpoint =
        config.endpoint.trimmed();

    if (endpoint.isEmpty()) {

        if (error) {

            *error =
                QStringLiteral(
                    "Remote LLM mode requires an endpoint."
                );
        }

        return false;
    }

    const QUrl url(
        endpoint
    );

    if (
        !url.isValid() ||
        url.isEmpty() ||
        url.scheme().isEmpty() ||
        url.host().isEmpty()
    ) {

        if (error) {

            *error =
                QStringLiteral(
                    "Remote LLM endpoint is invalid: %1"
                ).arg(
                    endpoint
                );
        }

        return false;
    }

    if (config.model.trimmed().isEmpty()) {

        if (error) {

            *error =
                QStringLiteral(
                    "Remote LLM mode requires a model ID."
                );
        }

        return false;
    }

    if (
        config.authType ==
            LlmAuthType::Bearer &&
        config.apiKey.trimmed().isEmpty()
    ) {

        if (error) {

            *error =
                QStringLiteral(
                    "Bearer-authenticated remote LLM mode requires an API key."
                );
        }

        return false;
    }

    return true;
}


bool InferenceService::configureRemoteLlm()
{
    QString validationError;

    if (
        !validateLlmConfig(
            m_llmConfig,
            &validationError
        )
    ) {

        emit serviceError(
            validationError
        );

        return false;
    }

    /*
     * Remote mode must never keep the local llama.cpp Docker
     * server running.
     */
    if (m_llamaManager) {

        m_llamaManager->stop();
    }

    m_llmReady =
        false;

    m_llmEndpoint =
        m_llmConfig.endpoint.trimmed();

    m_llmModel =
        m_llmConfig.model.trimmed();

    m_llmReady =
        true;

    qDebug()
        << "[InferenceService] Using remote LLM:"
        << m_llmEndpoint
        << "model="
        << m_llmModel
        << "auth="
        << (
            m_llmConfig.authType ==
                LlmAuthType::Bearer &&
            !m_llmConfig.apiKey.isEmpty()
        );

    emit llmConfigurationChanged();
    emit llmReady();

    return true;
}


bool InferenceService::setLlmConfig(
    const LlmConfig &config
)
{
    QString validationError;

    if (
        !validateLlmConfig(
            config,
            &validationError
        )
    ) {

        emit llmError(
            validationError
        );

        return false;
    }

    m_llmConfig =
        config;

    emit llmConfigurationChanged();

    if (!m_initialized) {
        return true;
    }

    if (
        m_llmConfig.mode ==
        LlmMode::Remote
    ) {

        return configureRemoteLlm();
    }

    /*
     * Switching to local mode.
     */
    m_llmReady =
        false;

    m_llmEndpoint.clear();
    m_llmModel.clear();

    if (!startSelectedLlmModel()) {

        emit llmError(
            QStringLiteral(
                "Unable to start the selected local LLM model."
            )
        );

        return false;
    }

    return true;
}


InferenceService::LlmMode
InferenceService::llmMode() const
{
    return m_llmConfig.mode;
}


QString InferenceService::llmEndpoint() const
{
    return m_llmEndpoint;
}


QString InferenceService::llmModel() const
{
    return m_llmModel;
}


// =============================================================================
// Models
// =============================================================================

ModelManager *
InferenceService::models() const
{
    return m_modelManager.get();
}


QString InferenceService::modelDirectory() const
{
    if (!m_modelManager)
        return QString();

    return m_modelManager
        ->storageDirectory();
}


bool InferenceService::setModelDirectory(
    const QString &directory
)
{
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
)
{
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
)
{
    if (m_modelManager) {

        m_modelManager
            ->inspectRemoteModel(
                repoId
            );
    }
}


QList<ModelManager::RemoteModel>
InferenceService::remoteLlmModels() const
{
    if (!m_modelManager)
        return {};

    return m_modelManager
        ->remoteModels();
}


QList<ModelManager::ModelVariant>
InferenceService::remoteLlmVariants(
    const QString &repoId
) const
{
    if (!m_modelManager)
        return {};

    return m_modelManager
        ->remoteVariants(
            repoId
        );
}


QString
InferenceService::selectedLlmModelId() const
{
    if (!m_modelManager)
        return QString();

    return m_modelManager
        ->selectedModelId();
}


ModelManager::ModelVariant
InferenceService::selectedLlmModel() const
{
    if (!m_modelManager)
        return {};

    return m_modelManager
        ->selectedModel();
}


bool InferenceService::selectLlmModel(
    const ModelManager::ModelVariant &variant
)
{
    if (!m_modelManager)
        return false;

    /*
     * ModelManager is still available in remote mode so clients
     * can browse/download local models. Selecting a model does
     * not start it unless local LLM mode is active.
     */
    return m_modelManager
        ->selectModel(
            variant
        );
}


bool InferenceService::selectLlmModel(
    const QString &modelPathOrId
)
{
    if (!m_modelManager)
        return false;

    return m_modelManager
        ->selectModel(
            modelPathOrId
        );
}


void InferenceService::downloadLlmModel(
    const ModelManager::ModelVariant &variant
)
{
    if (m_modelManager) {

        m_modelManager
            ->downloadModel(
                variant
            );
    }
}


void InferenceService::cancelModelDownload()
{
    if (m_modelManager)
        m_modelManager
            ->cancelDownload();
}


void InferenceService::onModelSelected(
    const QString &modelId
)
{
    emit selectedLlmModelChanged(
        modelId
    );

    if (!m_initialized)
        return;

    /*
     * Selecting a local model must never start Docker while the
     * active LLM is remote.
     */
    if (
        m_llmConfig.mode !=
        LlmMode::Local
    ) {
        return;
    }

    startSelectedLlmModel();
}


bool InferenceService::startSelectedLlmModel()
{
    if (
        m_llmConfig.mode !=
        LlmMode::Local
    ) {
        return false;
    }

    if (
        !m_modelManager ||
        !m_llamaManager
    ) {
        return false;
    }

    const QString selectedId =
        m_modelManager
            ->selectedModelId();

    if (selectedId.isEmpty())
        return false;

    QString modelPath;

    // Check if selectedId is a direct file path or filename
    // within the LLM directory.
    QFileInfo directInfo(
        selectedId
    );

    if (!directInfo.isAbsolute()) {

        directInfo.setFile(
            QDir(
                QFPaths::llmModelsDir()
            ).filePath(
                selectedId
            )
        );
    }

    if (
        directInfo.exists() &&
        directInfo.isFile() &&
        directInfo.isReadable()
    ) {

        modelPath =
            directInfo.absoluteFilePath();

    } else {

        const ModelManager::ModelVariant variant =
            m_modelManager->selectedModel();

        if (
            !variant.id.isEmpty() &&
            m_modelManager->isVariantInstalled(
                variant
            )
        ) {

            modelPath =
                m_modelManager->variantEntryPath(
                    variant
                );
        }
    }

    if (modelPath.isEmpty()) {

        qWarning()
            << "[InferenceService] Selected model has no valid local path:"
            << selectedId;

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

    m_llmModel =
        QStringLiteral(
            "/models/%1"
        ).arg(
            modelInfo.fileName()
        );

    qDebug()
        << "[InferenceService] Starting selected local model:"
        << selectedId;

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


// =============================================================================
// LLM
// =============================================================================

void InferenceService::sendChatRequest(
    const QJsonArray &messages,
    const QString &model,
    double temperature,
    int timeoutMs,
    const QString &grammar,
    const QJsonObject &responseFormat
)
{
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

    request.model =
        model.isEmpty()
            ? m_llmModel
            : model;

    request.messages =
        messages;

    request.temperature =
        temperature;

    request.timeoutMs =
        timeoutMs;

    /*
     * GBNF is only meaningful to the local llama.cpp backend.
     */
    if (
        m_llmConfig.mode ==
        LlmMode::Local
    ) {

        request.grammar =
            grammar;
    }

    /*
     * JSON Schema structured output is for remote
     * OpenAI-compatible providers such as OpenRouter.
     */
    if (
        m_llmConfig.mode ==
        LlmMode::Remote
    ) {

        request.responseFormat =
            responseFormat;
    }

    if (
        m_llmConfig.mode ==
        LlmMode::Remote
    ) {

        request.authType =
            m_llmConfig.authType ==
                LlmAuthType::Bearer
                ? LlmClient::AuthType::Bearer
                : LlmClient::AuthType::None;

        request.apiKey =
            m_llmConfig.apiKey;
    }

    qDebug()
        << "[InferenceService] Sending chat request"
        << "mode="
        << (
            m_llmConfig.mode ==
                LlmMode::Local
                ? QStringLiteral("local")
                : QStringLiteral("remote")
        )
        << "model="
        << request.model
        << "messages="
        << request.messages.size()
        << "temperature="
        << request.temperature
        << "timeoutMs="
        << request.timeoutMs
        << "hasGrammar="
        << !request.grammar.isEmpty()
        << "hasResponseFormat="
        << !request.responseFormat.isEmpty()
        << "authenticated="
        << (
            request.authType ==
                LlmClient::AuthType::Bearer &&
            !request.apiKey.isEmpty()
        );

    m_llmClient->sendRequest(
        request
    );
}


void InferenceService::abortChatRequest()
{
    if (m_llmClient)
        m_llmClient->abortRequest();
}


bool InferenceService::isLlmReady() const
{
    return m_llmReady;
}


void InferenceService::onLlmServerReady()
{
    /*
     * A serverReady signal only matters for local mode.
     */
    if (
        m_llmConfig.mode !=
        LlmMode::Local
    ) {
        return;
    }

    m_llmReady =
        true;

    emit llmReady();

    qDebug()
        << "[InferenceService] Local LLM ready:"
        << m_llmEndpoint
        << "model="
        << m_llmModel;
}


void InferenceService::onLlamaError(
    const QString &error
)
{
    if (
        m_llmConfig.mode !=
        LlmMode::Local
    ) {
        return;
    }

    m_llmReady =
        false;

    emit llmError(
        error
    );

    emit serviceError(
        error
    );
}


// =============================================================================
// STT
// =============================================================================

QString InferenceService::transcribe(
    const std::vector<float> &pcm32f
)
{
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


bool InferenceService::isSttReady() const
{
    return m_sttReady;
}


InferenceService::SttModel
InferenceService::sttModel() const
{
    return m_sttModel;
}


QString InferenceService::sttModelName() const
{
    return sttModelToString(
        m_sttModel
    );
}


QString InferenceService::sttModelPath() const
{
    return m_sttModelPath;
}


bool InferenceService::setSttModel(
    SttModel model
)
{
    if (m_sttModel == model)
        return true;

    m_sttModel =
        model;

    m_sttReady =
        false;

    m_stt.reset();

    const QString path =
        resolveSttModelPath(
            QString()
        );

    if (path.isEmpty()) {

        emit serviceError(
            QStringLiteral(
                "STT model is not installed: %1"
            ).arg(
                sttModelName()
            )
        );

        return false;
    }

    const QString language =
        qEnvironmentVariable(
            "QF_STT_LANGUAGE"
        ).trimmed();

    const int gpu =
        resolveSttGpu();

    auto transcriber =
        std::make_unique<NemoTranscriber>(
            path,
            gpu,
            language,
            this
        );

    if (!transcriber->isLoaded()) {

        emit serviceError(
            QStringLiteral(
                "Failed to load STT model: %1"
            ).arg(
                path
            )
        );

        return false;
    }

    connect(
        transcriber.get(),
        &NemoTranscriber::transcriptionFinished,
        this,
        &InferenceService::transcriptionFinished
    );

    connect(
        transcriber.get(),
        &NemoTranscriber::transcriptionError,
        this,
        [this](const QString &error) {

            emit transcriptionError(
                error
            );

            emit serviceError(
                error
            );
        }
    );

    m_stt =
        std::move(
            transcriber
        );

    m_sttModelPath =
        path;

    m_sttReady =
        true;

    emit sttModelChanged(
        sttModelName()
    );

    return true;
}


bool InferenceService::setSttModelName(
    const QString &model
)
{
    const QString normalized =
        model.trimmed().toLower();

    if (normalized.isEmpty())
        return false;

    const SttModel selected =
        sttModelFromString(
            normalized
        );

    if (
        normalized !=
            QStringLiteral("nemotron-3.5") &&
        normalized !=
            QStringLiteral("nemotron-en") &&
        normalized !=
            QStringLiteral("parakeet-tdt") &&
        normalized !=
            QStringLiteral("parakeet-ctc")
    ) {

        emit serviceError(
            QStringLiteral(
                "Unknown STT model '%1'."
            ).arg(
                model
            )
        );

        return false;
    }

    return setSttModel(
        selected
    );
}


QStringList InferenceService::availableSttModels() const
{
    return {
        QStringLiteral(
            "nemotron-3.5"
        ),
        QStringLiteral(
            "nemotron-en"
        ),
        QStringLiteral(
            "parakeet-tdt"
        ),
        QStringLiteral(
            "parakeet-ctc"
        )
    };
}


// =============================================================================
// TTS
// =============================================================================

bool InferenceService::isTtsReady() const
{
    return m_ttsReady;
}


bool InferenceService::isTtsEnabled() const
{
    if (!m_ttsManager)
        return false;

    return m_ttsManager
        ->isEnabled();
}


void InferenceService::setTtsEnabled(
    bool enabled
)
{
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
)
{
    if (
        !m_ttsManager ||
        !m_ttsReady ||
        !m_ttsManager->isEnabled()
    ) {
        return;
    }

    m_ttsManager
        ->enqueueSentence(
            text,
            speakerId
        );
}


void InferenceService::stopSpeech()
{
    if (m_ttsManager)
        m_ttsManager
            ->stopAndClear();
}


QString InferenceService::ttsVoice() const
{
    if (!m_ttsManager)
        return QString();

    return m_ttsManager
        ->voice();
}


void InferenceService::setTtsVoice(
    const QString &voice
)
{
    if (m_ttsManager) {

        m_ttsManager
            ->setVoice(
                voice
            );
    }
}


QStringList InferenceService::ttsVoices() const
{
    if (!m_ttsManager)
        return {};

    return m_ttsManager
        ->availableVoices();
}


void InferenceService::refreshTtsVoices()
{
    if (m_ttsManager)
        m_ttsManager
            ->refreshVoices();
}


void InferenceService::onTtsServerReady()
{
    m_ttsReady =
        true;

    emit ttsReady();

    qDebug()
        << "[InferenceService] TTS ready.";
}


// =============================================================================
// STT model helpers
// =============================================================================

QString InferenceService::resolveSttModelFilename(
    SttModel model
) const
{
    switch (model) {

        case SttModel::Nemotron35:
            return QStringLiteral(
                "nemotron-3.5-asr-streaming-0.6b.q8_0.gguf"
            );

        case SttModel::NemotronEnglish:
            return QStringLiteral(
                "nemotron-speech-streaming-en-0.6b.q8_0.gguf"
            );

        case SttModel::ParakeetTdt:
            return QStringLiteral(
                "parakeet-tdt-0.6b-v3.q8_0.gguf"
            );

        case SttModel::ParakeetCtc:
            return QStringLiteral(
                "parakeet-ctc-1.1b.q8_0.gguf"
            );
    }

    return QString();
}


QString InferenceService::sttModelToString(
    SttModel model
) const
{
    switch (model) {

        case SttModel::Nemotron35:
            return QStringLiteral(
                "nemotron-3.5"
            );

        case SttModel::NemotronEnglish:
            return QStringLiteral(
                "nemotron-en"
            );

        case SttModel::ParakeetTdt:
            return QStringLiteral(
                "parakeet-tdt"
            );

        case SttModel::ParakeetCtc:
            return QStringLiteral(
                "parakeet-ctc"
            );
    }

    return QStringLiteral(
        "nemotron-3.5"
    );
}


InferenceService::SttModel
InferenceService::sttModelFromString(
    const QString &model
) const
{
    const QString normalized =
        model.trimmed().toLower();

    if (
        normalized ==
        QStringLiteral(
            "nemotron-en"
        )
    ) {

        return SttModel::NemotronEnglish;
    }

    if (
        normalized ==
        QStringLiteral(
            "parakeet-tdt"
        )
    ) {

        return SttModel::ParakeetTdt;
    }

    if (
        normalized ==
        QStringLiteral(
            "parakeet-ctc"
        )
    ) {

        return SttModel::ParakeetCtc;
    }

    return SttModel::Nemotron35;
}


int InferenceService::resolveSttGpu() const
{
    const QString value =
        qEnvironmentVariable(
            "QF_STT_GPU",
            "0"
        ).trimmed();

    bool ok = false;

    const int gpu =
        value.toInt(
            &ok
        );

    if (!ok)
        return 0;

    return gpu;
}


QString InferenceService::resolveSttModelPath(
    const QString &requestedPath
) const
{
    // -------------------------------------------------------------------------
    // 1. Explicit model path argument
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

            qDebug()
                << "[InferenceService] Using requested STT model:"
                << info.absoluteFilePath();

            return info.absoluteFilePath();
        }

        qWarning()
            << "[InferenceService] Requested STT model is invalid:"
            << requestedPath;
    }

    // -------------------------------------------------------------------------
    // 2. Explicit environment path override
    // -------------------------------------------------------------------------

    const QString environmentPath =
        qEnvironmentVariable(
            "QF_STT_MODEL_PATH"
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
                << "[InferenceService] Using QF_STT_MODEL_PATH:"
                << info.absoluteFilePath();

            return info.absoluteFilePath();
        }

        qWarning()
            << "[InferenceService] QF_STT_MODEL_PATH is invalid:"
            << environmentPath;
    }

    // -------------------------------------------------------------------------
    // 3. Shared machine-wide model cache
    // -------------------------------------------------------------------------

    const QString filename =
        resolveSttModelFilename(
            m_sttModel
        );

    if (filename.isEmpty())
        return QString();

    const QString modelPath =
        QDir(
            QFPaths::sttModelsDir()
        ).filePath(
            filename
        );

    const QFileInfo info(
        modelPath
    );

    if (
        info.exists() &&
        info.isFile() &&
        info.isReadable()
    ) {

        qDebug()
            << "[InferenceService] Using shared STT model:"
            << modelPath;

        return info.absoluteFilePath();
    }

    qWarning()
        << "[InferenceService] STT model not found:"
        << modelPath;

    return QString();
}


// =============================================================================
// OCR
// =============================================================================

QString InferenceService::extractText(
    const QImage &image
)
{
    Q_UNUSED(image)

    return {};
}