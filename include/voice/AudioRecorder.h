#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QByteArray>
#include <QDebug>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QTimer>
#include <cmath>
#include <vector>

class AudioRecorder : public QObject {
  Q_OBJECT

public:
  explicit AudioRecorder(QObject *parent = nullptr) : QObject(parent) {
    m_format.setSampleRate(16000);
    m_format.setChannelCount(1);
    m_format.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice device = QMediaDevices::defaultAudioInput();

    if (device.isNull()) {
      qWarning() << "[Audio] No default input device found";
      return;
    }

    qDebug() << "[Audio] Default input device:" << device.description();

    setInputDevice(device);

    QTimer::singleShot(100, this, [this]() { startListening(); });
  }

  ~AudioRecorder() override { stopListening(); }

  QList<QAudioDevice> availableInputDevices() const {
    return QMediaDevices::audioInputs();
  }

  QAudioDevice currentDevice() const { return m_currentDevice; }

  void setInputDevice(const QAudioDevice &device) {
    if (device.isNull()) {
      qWarning() << "[Audio] Tried to set null device";

      return;
    }

    qDebug() << "[Audio] Setting input device:" << device.description();

    // Fully tear down the old source.
    if (m_audioSource) {
      m_audioSource->stop();
      m_audioSource->deleteLater();
      m_audioSource = nullptr;
    }

    m_audioDevice = nullptr;

    m_currentDevice = device;

    // Prefer 16 kHz mono Int16 because this is what the STT pipeline
    // expects.
    QAudioFormat requestedFormat;

    requestedFormat.setSampleRate(16000);
    requestedFormat.setChannelCount(1);
    requestedFormat.setSampleFormat(QAudioFormat::Int16);

    if (device.isFormatSupported(requestedFormat)) {
      m_format = requestedFormat;
    } else {
      qWarning() << "[Audio] 16 kHz Int16 is not supported by"
                 << device.description() << "- using preferred format";

      m_format = device.preferredFormat();

      // Keep mono when possible.
      m_format.setChannelCount(1);

      // Try to keep Int16, but don't force an unsupported format.
      if (m_format.sampleFormat() != QAudioFormat::Int16) {
        qWarning() << "[Audio] Preferred format is not Int16:"
                   << m_format.sampleFormat();
      }
    }

    qDebug() << "[Audio] Format:"
             << "sampleRate=" << m_format.sampleRate()
             << "channels=" << m_format.channelCount()
             << "sampleFormat=" << m_format.sampleFormat();

    m_audioSource = new QAudioSource(device, m_format, this);

    m_audioSource->setBufferSize(4096);

    m_audioSource->setVolume(1.0);

    qDebug() << "[Audio] Created QAudioSource:" << m_audioSource;

    qDebug() << "[Audio] Device:" << device.description();
  }

  void startListening() {
    if (!m_audioSource) {
      qWarning() << "[Audio] startListening - no source";

      // Try to recover automatically if the default device exists.
      const QAudioDevice device = QMediaDevices::defaultAudioInput();

      if (device.isNull()) {
        qWarning() << "[Audio] No default input device available";

        return;
      }

      qDebug() << "[Audio] Recreating source for:" << device.description();

      setInputDevice(device);

      if (!m_audioSource) {
        qWarning() << "[Audio] Failed to recreate QAudioSource";

        return;
      }
    }

    // If the source is already running, leave it alone.
    const QAudio::State currentState = m_audioSource->state();

    if (currentState == QAudio::ActiveState ||
        currentState == QAudio::IdleState) {
      qDebug() << "[Audio] Already listening, state:" << currentState;

      return;
    }

    if (currentState != QAudio::StoppedState) {
      m_audioSource->stop();
    }

    m_audioDevice = m_audioSource->start();

    qDebug() << "[Audio] start() returned:" << m_audioDevice;

    qDebug() << "[Audio] State:" << m_audioSource->state()
             << "error:" << m_audioSource->error()
             << "device:" << m_currentDevice.description()
             << "volume:" << m_audioSource->volume();

    if (!m_audioDevice) {
      qWarning() << "[Audio] start() returned null QIODevice";

      qWarning() << "[Audio] QAudioSource error:" << m_audioSource->error();

      return;
    }

    connect(m_audioDevice, &QIODevice::readyRead, this,
            &AudioRecorder::onAudioDataReady, Qt::UniqueConnection);

    qDebug() << "[Audio] readyRead connected"
             << "bytesAvailable=" << m_audioDevice->bytesAvailable();
  }

  void startRecording() {
    startListening();
    startBufferingSpeech();
  }

  std::vector<float> stopRecording() {
    std::vector<float> pcmData = stopBufferingSpeech();

    stopListening();

    return pcmData;
  }

  void stopListening() {
    if (!m_audioSource) {
      m_audioDevice = nullptr;
      return;
    }

    m_audioSource->stop();

    m_audioDevice = nullptr;

    qDebug() << "[Audio] Listening stopped";
  }

  void startBufferingSpeech() {
    QMutexLocker locker(&m_mutex);

    m_isCapturingSpeech = true;

    m_speechBuffer.clear();

    m_recentSamples.clear();
  }

  std::vector<float> stopBufferingSpeech() {
    QMutexLocker locker(&m_mutex);

    m_isCapturingSpeech = false;

    const int sampleCount =
        m_speechBuffer.size() / static_cast<int>(sizeof(int16_t));

    if (sampleCount <= 0) {
      m_speechBuffer.clear();
      m_recentSamples.clear();

      return {};
    }

    const auto *samples16 =
        reinterpret_cast<const int16_t *>(m_speechBuffer.constData());

    std::vector<float> pcm32f(static_cast<size_t>(sampleCount));

    for (int i = 0; i < sampleCount; ++i) {
      pcm32f[static_cast<size_t>(i)] =
          static_cast<float>(samples16[i]) / 32768.0f;
    }

    m_speechBuffer.clear();
    m_recentSamples.clear();

    return pcm32f;
  }

  std::vector<float> getRecentSamples(size_t maxSamples = 1600) {
    QMutexLocker locker(&m_mutex);

    if (m_recentSamples.empty()) {
      return {};
    }

    if (m_recentSamples.size() <= maxSamples) {
      return m_recentSamples;
    }

    return std::vector<float>(m_recentSamples.end() -
                                  static_cast<std::ptrdiff_t>(maxSamples),
                              m_recentSamples.end());
  }

signals:
  void audioChunkReady(const std::vector<float> &chunk);

private slots:
  void onAudioDataReady() {
    if (!m_audioDevice) {
      return;
    }

    const QByteArray rawData = m_audioDevice->readAll();

    if (rawData.isEmpty()) {
      return;
    }

    QMutexLocker locker(&m_mutex);

    if (m_isCapturingSpeech) {
      m_speechBuffer.append(rawData);
    }

    const int sampleCount = rawData.size() / static_cast<int>(sizeof(int16_t));

    if (sampleCount <= 0) {
      return;
    }

    const auto *samples16 =
        reinterpret_cast<const int16_t *>(rawData.constData());

    std::vector<float> chunk(static_cast<size_t>(sampleCount));

    for (int i = 0; i < sampleCount; ++i) {
      const int16_t sample = samples16[i];

      const float floatSample = static_cast<float>(sample) / 32768.0f;

      chunk[static_cast<size_t>(i)] = floatSample;

      m_recentSamples.push_back(floatSample);
    }

    // Keep approximately one second of recent audio at 16 kHz.
    constexpr size_t maxRecentBufferSize = 16000;

    if (m_recentSamples.size() > maxRecentBufferSize) {
      const size_t removeCount = m_recentSamples.size() - maxRecentBufferSize;

      m_recentSamples.erase(m_recentSamples.begin(),
                            m_recentSamples.begin() +
                                static_cast<std::ptrdiff_t>(removeCount));
    }

    emit audioChunkReady(chunk);
  }

private:
  QAudioFormat m_format;

  QAudioSource *m_audioSource = nullptr;

  QIODevice *m_audioDevice = nullptr;

  QAudioDevice m_currentDevice;

  QByteArray m_speechBuffer;

  std::vector<float> m_recentSamples;

  bool m_isCapturingSpeech = false;

  QMutex m_mutex;
};
