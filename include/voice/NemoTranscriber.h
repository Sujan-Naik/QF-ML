#pragma once

#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

#include <atomic>
#include <mutex>
#include <vector>

#include "ITranscriber.h"
#include "nemo_speech/asr.h"

class QThread;

// Worker that runs the streaming push/pull loop on its own thread.
// Owns the stream handle. The transcriber hands it audio and reads
// results through queued signals.
class NemoStreamWorker : public QObject {
  Q_OBJECT

public:
  NemoStreamWorker(nemo_speech_asr_recognizer *recognizer,
                   int32_t rnntRightContext, QObject *parent = nullptr);
  ~NemoStreamWorker() override;

  void enqueueAudio(const std::vector<float> &pcm);
  void requestStop();
  bool isRunning() const;

signals:
  void streamOpened();
  void streamFailed(const QString &error);
  // Emitted for every result. isFinal distinguishes a finalized
  // utterance from an interim that will be replaced.
  void segmentResult(const QString &text, bool isFinal);
  void streamClosed();

public slots:
  void run();

private:
  void processAvailableResults();

  nemo_speech_asr_recognizer *m_recognizer = nullptr;
  nemo_speech_asr_stream *m_stream = nullptr;
  int32_t m_rnntRightContext = 1;

  mutable std::mutex m_mutex;
  std::vector<float> m_pendingAudio;
  std::atomic<bool> m_stopRequested{false};
  std::atomic<bool> m_running{false};
};

class NemoTranscriber : public QObject, public ITranscriber {
  Q_OBJECT

public:
  explicit NemoTranscriber(const QString &modelPath, int gpu = 0,
                           const QString &language = QString(),
                           QObject *parent = nullptr);

  ~NemoTranscriber() override;

  bool isLoaded() const override;

  QString transcribe(const std::vector<float> &pcm32f) override;

  QString modelPath() const;
  QString language() const;
  void setLanguage(const QString &language);

  bool startStreaming(int32_t rightContext = 1);
  void feedAudio(const std::vector<float> &pcm);
  void stopStreaming();
  bool isStreaming() const;

signals:
  void transcriptionFinished(const QString &text);
  void transcriptionError(const QString &error);

  // Emitted for every streaming result on the main thread.
  // isFinal is true when the utterance is complete and the text will
  // not change again; false while the runtime may still refine it.
  void liveSegment(const QString &text, bool isFinal);

  void streamOpened();
  void streamClosed();

private:
  nemo_speech_asr_recognizer *m_recognizer = nullptr;

  QString m_modelPath;
  QString m_language;

  nemo_speech_asr_streaming_config m_streamingConfig = {};

  QThread *m_workerThread = nullptr;
  NemoStreamWorker *m_worker = nullptr;

  QTcpServer m_httpServer;
};