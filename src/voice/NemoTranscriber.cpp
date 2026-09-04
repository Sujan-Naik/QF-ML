#include "../../include/voice/NemoTranscriber.h"

#include <QByteArray>
#include <QDebug>

NemoTranscriber::NemoTranscriber(
    const QString &modelPath,
    int gpu,
    const QString &language,
    QObject *parent
)
    : QObject(parent)
    , m_modelPath(modelPath)
    , m_language(language.trimmed()) {
    const QByteArray path = modelPath.toUtf8();

    nemo_speech_asr_backend_config backend = {};
    backend.size = sizeof(backend);
    backend.gpu = gpu;

    nemo_speech_asr_model_config model = {};
    model.size = sizeof(model);
    model.path = path.constData();

    nemo_speech_asr_recognizer_config config = {};
    config.size = sizeof(config);
    config.backend = &backend;
    config.model = &model;

    const nemo_speech_asr_status status =
        nemo_speech_asr_create(
            &config,
            &m_recognizer
        );

    if (status != NEMO_SPEECH_ASR_OK || !m_recognizer) {
        const char *error = nemo_speech_asr_last_error();

        const QString errorText =
            error && *error
                ? QString::fromUtf8(error)
                : QStringLiteral(
                    "Failed to create NeMo-Speech ASR recognizer."
                );

        qWarning()
            << "[NemoTranscriber] Failed to load model:"
            << modelPath
            << errorText;

        m_recognizer = nullptr;
        return;
    }

    qDebug()
        << "[NemoTranscriber] Loaded model:"
        << modelPath
        << "GPU:"
        << gpu;
}

NemoTranscriber::~NemoTranscriber() {
    if (m_recognizer) {
        nemo_speech_asr_destroy(
            m_recognizer
        );

        m_recognizer = nullptr;
    }
}

bool NemoTranscriber::isLoaded() const {
    return m_recognizer != nullptr;
}

QString NemoTranscriber::transcribe(
    const std::vector<float> &pcm32f
) {
    if (!m_recognizer) {
        const QString errorText =
            QStringLiteral(
                "NeMo-Speech recognizer is not loaded."
            );

        qWarning()
            << "[NemoTranscriber]"
            << errorText;

        emit transcriptionError(
            errorText
        );

        return {};
    }

    if (pcm32f.empty())
        return {};

    nemo_speech_asr_recognition_options options =
        nemo_speech_asr_recognition_options_default();

    options.enable_automatic_punctuation = true;

    /*
     * NeMo-Speech.cpp accepts mono float32 PCM and supports
     * input rates from 8 kHz to 96 kHz, resampling internally.
     *
     * Your current audio pipeline is 16 kHz, so pass 16000.
     */
    constexpr int32_t sampleRate = 16000;

    nemo_speech_asr_result *result = nullptr;

    const nemo_speech_asr_status status =
        nemo_speech_asr_recognize_f32(
            m_recognizer,
            &options,
            pcm32f.data(),
            pcm32f.size(),
            sampleRate,
            &result
        );

    if (status != NEMO_SPEECH_ASR_OK || !result) {
        const char *error =
            nemo_speech_asr_last_error();

        const QString errorText =
            error && *error
                ? QString::fromUtf8(error)
                : QStringLiteral(
                    "Unknown NeMo-Speech transcription error."
                );

        qWarning()
            << "[NemoTranscriber] Transcription failed:"
            << errorText;

        emit transcriptionError(
            errorText
        );

        return {};
    }

    const char *transcript =
        nemo_speech_asr_result_transcript(
            result,
            0
        );

    const QString text =
        transcript
            ? QString::fromUtf8(transcript).trimmed()
            : QString{};

    nemo_speech_asr_result_destroy(
        result
    );

    if (text.isEmpty())
        return {};

    emit transcriptionFinished(
        text
    );

    return text;
}
