#include "../../include/voice/ParakeetTranscriber.h"

#include <QByteArray>
#include <QDebug>

#include "parakeet_capi.h"


ParakeetTranscriber::ParakeetTranscriber(
    const QString &modelPath,
    QObject *parent
)
    : QObject(parent) {
    const QByteArray path =
            modelPath.toUtf8();

    m_ctx =
            parakeet_capi_load(
                path.constData()
            );

    if (!m_ctx) {
        qWarning()
                << "[ParakeetTranscriber] Failed to load model:"
                << modelPath;

        return;
    }

    qDebug()
            << "[ParakeetTranscriber] Loaded model:"
            << modelPath;
}


ParakeetTranscriber::~ParakeetTranscriber() {
    if (m_ctx) {
        parakeet_capi_free(
            m_ctx
        );

        m_ctx = nullptr;
    }
}


bool ParakeetTranscriber::isLoaded() const {
    return m_ctx != nullptr;
}


QString ParakeetTranscriber::transcribe(
    const std::vector<float> &pcm32f
) {
    if (!m_ctx) {
        qWarning()
                << "[ParakeetTranscriber]"
                << "transcribe() called without loaded model.";

        return {};
    }

    if (pcm32f.empty())
        return {};

    /*
     * parakeet.cpp expects mono float PCM.
     *
     * The C API resamples to 16 kHz automatically when the supplied
     * sample rate differs from 16000.
     *
     * Decoder:
     *   0 = model default
     *   1 = CTC
     *   2 = TDT/RNNT
     *
     * We're using the Parakeet TDT 1.1B model, so use decoder 2.
     */

    constexpr int sampleRate = 16000;
    constexpr int decoder = 2;

    const int sampleCount =
            static_cast<int>(
                pcm32f.size()
            );

    char *text =
            parakeet_capi_transcribe_pcm(
                m_ctx,
                pcm32f.data(),
                sampleCount,
                sampleRate,
                decoder
            );

    if (!text) {
        const char *error =
                parakeet_capi_last_error(
                    m_ctx
                );

        const QString errorText =
                error && *error
                    ? QString::fromUtf8(error)
                    : QStringLiteral(
                        "Unknown Parakeet transcription error."
                    );

        qWarning()
                << "[ParakeetTranscriber] Transcription failed:"
                << errorText;

        emit transcriptionError(
            errorText
        );

        return {};
    }

    const QString result =
            QString::fromUtf8(
                text
            ).trimmed();

    parakeet_capi_free_string(
        text
    );

    return result;
}
