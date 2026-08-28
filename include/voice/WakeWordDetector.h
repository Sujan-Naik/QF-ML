#pragma once

#include <QObject>
#include <QString>
#include <QDebug>
#include <QTimer>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <onnxruntime_cxx_api.h>

class WakeWordDetector : public QObject {
    Q_OBJECT

public:
    enum State {
        Initializing,
        Ready,
        Detected
    };

    explicit WakeWordDetector(const QString &melModelPath,
                              const QString &embModelPath,
                              const QString &wwModelPath,
                              float threshold = 0.5f,
                              QObject *parent = nullptr)
        : QObject(parent), m_threshold(threshold), m_state(Initializing) {
        m_env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "OpenWakeWordEngine");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);

#ifdef _WIN32
        m_melSession = std::make_unique<Ort::Session>(m_env, melModelPath.toStdWString().c_str(), sessionOptions);
        m_embSession = std::make_unique<Ort::Session>(m_env, embModelPath.toStdWString().c_str(), sessionOptions);
        m_wwSession = std::make_unique<Ort::Session>(m_env, wwModelPath.toStdWString().c_str(), sessionOptions);
#else
        m_melSession = std::make_unique<Ort::Session>(m_env, melModelPath.toStdString().c_str(), sessionOptions);
        m_embSession = std::make_unique<Ort::Session>(m_env, embModelPath.toStdString().c_str(), sessionOptions);
        m_wwSession = std::make_unique<Ort::Session>(m_env, wwModelPath.toStdString().c_str(), sessionOptions);
#endif

        Ort::AllocatorWithDefaultOptions allocator;

        m_melInputName = m_melSession->GetInputNameAllocated(0, allocator).get();
        m_melOutputName = m_melSession->GetOutputNameAllocated(0, allocator).get();

        m_embInputName = m_embSession->GetInputNameAllocated(0, allocator).get();
        m_embOutputName = m_embSession->GetOutputNameAllocated(0, allocator).get();

        m_wwInputName = m_wwSession->GetInputNameAllocated(0, allocator).get();
        m_wwOutputName = m_wwSession->GetOutputNameAllocated(0, allocator).get();
    }

    void reset() {
        m_ignoreAudio.store(false);
        m_state = Ready;
        m_melWindow.clear();
        m_embeddingWindow.clear();
        m_audioBuffer.clear();
        qDebug() << "[WW] Detector reset, listening again";
    }

public slots:
    void processAudioChunk(const std::vector<float> &chunk) {
        if (m_ignoreAudio.load()) {
            return;
        }

        if (chunk.empty()) {
            return;
        }

        std::vector<float> processedChunk;
        processedChunk.reserve(chunk.size());
        for (float sample: chunk) {
            processedChunk.push_back(sample * 32768.0f);
        }

        m_audioBuffer.insert(m_audioBuffer.end(), processedChunk.begin(), processedChunk.end());

        const size_t FRAME_SIZE = 1280;
        while (m_audioBuffer.size() >= FRAME_SIZE) {
            if (m_ignoreAudio.load()) {
                return;
            }

            std::vector<float> frame(m_audioBuffer.begin(), m_audioBuffer.begin() + FRAME_SIZE);
            m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + FRAME_SIZE);

            float maxVal = 0.0f;
            for (float v: frame) {
                if (std::abs(v) > maxVal) maxVal = std::abs(v);
            }

            try {
                float confidence = predictFrame(frame);

                //                if (maxVal >= 500.0f) {
                //                    qDebug() << "[WW] conf:" << confidence
                //                             << "melWin:" << m_melWindow.size()
                //                             << "embWin:" << m_embeddingWindow.size()
                //                             << "maxVal:" << maxVal;
                //                }

                if (m_state == Ready && confidence >= m_threshold) {
                    m_ignoreAudio.store(true);
                    m_state = Detected;
                    emit wakeWordDetected(confidence);
                    return;
                }
            } catch (const std::exception &e) {
                qWarning() << "Frame prediction failed:" << e.what();
            }
        }
    }

signals:
    void wakeWordDetected(float confidence);

private:
    float predictFrame(const std::vector<float> &frame1280) {
        if (frame1280.size() != 1280) {
            throw std::runtime_error("Invalid frame size");
        }

        std::vector<float> frameCopy(frame1280.begin(), frame1280.end());
        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        try {
            int64_t melInputShape[] = {1, 1280};
            Ort::Value melInputTensor = Ort::Value::CreateTensor<float>(
                memoryInfo, frameCopy.data(), frameCopy.size(), melInputShape, 2);

            const char *melInputNames[] = {m_melInputName.c_str()};
            const char *melOutputNames[] = {m_melOutputName.c_str()};

            auto melOutputs = m_melSession->Run(
                Ort::RunOptions{nullptr}, melInputNames, &melInputTensor, 1, melOutputNames, 1);

            float *melData = melOutputs[0].GetTensorMutableData<float>();
            size_t melSize = melOutputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

            if (melSize == 0) {
                throw std::runtime_error("Mel output size is zero");
            }

            std::vector<float> scaledMels(melSize);
            for (size_t i = 0; i < melSize; ++i) {
                scaledMels[i] = (melData[i] / 10.0f) + 2.0f;
            }

            size_t numMelFrames = melSize / 32;
            if (numMelFrames == 0) numMelFrames = 1;

            for (size_t f = 0; f < numMelFrames; ++f) {
                std::vector<float> frame(32, 0.0f);
                size_t bins = std::min<size_t>(32, melSize - f * 32);
                for (size_t b = 0; b < bins; ++b) {
                    frame[b] = scaledMels[f * 32 + b];
                }
                m_melWindow.push_back(std::move(frame));
                if (m_melWindow.size() > 76) {
                    m_melWindow.pop_front();
                }
            }

            if (m_melWindow.size() < 76) {
                return 0.0f;
            }

            std::vector<float> flattenedMels;
            flattenedMels.reserve(76 * 32);
            for (const auto &melFrame: m_melWindow) {
                flattenedMels.insert(flattenedMels.end(), melFrame.begin(), melFrame.end());
            }

            int64_t embInputShape[] = {1, 76, 32, 1};
            Ort::Value embInputTensor = Ort::Value::CreateTensor<float>(
                memoryInfo, flattenedMels.data(), flattenedMels.size(), embInputShape, 4);

            const char *embInputNames[] = {m_embInputName.c_str()};
            const char *embOutputNames[] = {m_embOutputName.c_str()};

            auto embOutputs = m_embSession->Run(
                Ort::RunOptions{nullptr}, embInputNames, &embInputTensor, 1, embOutputNames, 1);

            float *embData = embOutputs[0].GetTensorMutableData<float>();
            size_t embSize = embOutputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

            if (embSize == 0) {
                throw std::runtime_error("Embedding output size is zero");
            }

            std::vector<float> singleEmbedding(embData, embData + embSize);
            m_embeddingWindow.push_back(singleEmbedding);
            if (m_embeddingWindow.size() > 16) {
                m_embeddingWindow.pop_front();
            }

            if (m_embeddingWindow.size() < 16) {
                return 0.0f;
            }

            if (m_state == Initializing && m_embeddingWindow.size() == 16) {
                m_state = Ready;
            }

            std::vector<float> flattenedEmbeddings;
            flattenedEmbeddings.reserve(16 * embSize);
            for (const auto &embFrame: m_embeddingWindow) {
                flattenedEmbeddings.insert(flattenedEmbeddings.end(), embFrame.begin(), embFrame.end());
            }

            int64_t wwInputShape[] = {1, 16, static_cast<int64_t>(embSize)};
            Ort::Value wwInputTensor = Ort::Value::CreateTensor<float>(
                memoryInfo, flattenedEmbeddings.data(), flattenedEmbeddings.size(), wwInputShape, 3);

            const char *wwInputNames[] = {m_wwInputName.c_str()};
            const char *wwOutputNames[] = {m_wwOutputName.c_str()};

            auto wwOutputs = m_wwSession->Run(
                Ort::RunOptions{nullptr}, wwInputNames, &wwInputTensor, 1, wwOutputNames, 1);

            const float *outputData = wwOutputs[0].GetTensorData<float>();
            if (!outputData) {
                throw std::runtime_error("Wake word output is null");
            }

            return outputData[0];
        } catch (const Ort::Exception &e) {
            throw std::runtime_error(std::string("ONNX error: ") + e.what());
        }
    }

    float m_threshold;
    State m_state;
    std::atomic<bool> m_ignoreAudio{false};
    Ort::Env m_env;
    std::unique_ptr<Ort::Session> m_melSession;
    std::unique_ptr<Ort::Session> m_embSession;
    std::unique_ptr<Ort::Session> m_wwSession;

    std::string m_melInputName, m_melOutputName;
    std::string m_embInputName, m_embOutputName;
    std::string m_wwInputName, m_wwOutputName;

    std::vector<float> m_audioBuffer;
    std::deque<std::vector<float> > m_melWindow;
    std::deque<std::vector<float> > m_embeddingWindow;
};
