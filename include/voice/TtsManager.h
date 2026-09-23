#ifndef TTSMANAGER_H
#define TTSMANAGER_H

#include "VisemeMap.h"

#include <QAudioSink>
#include <QBuffer>
#include <QDateTime>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QQueue>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <memory>

// TtsManager talks to a HeadTTS server over HTTP. HeadTTS runs the
// timestamped Kokoro ONNX model and returns audio, Oculus visemes, and
// per-viseme timing in one response.
//
// The server is not started by this class. Start it separately:
//
//   cd external/HeadTTS
//   npm start
//
// It listens on http://127.0.0.1:8882 by default.
class TtsManager : public QObject {
  Q_OBJECT

public:
  explicit TtsManager(QObject *parent = nullptr);

  ~TtsManager() override;

  // Connect to a HeadTTS server. Returns false if the URL is invalid.
  // The server is not probed here; the first synthesis request will
  // fail if it is not running.
  bool initialize(const QString &serverUrl = QString());

  void setEnabled(bool enabled);

  [[nodiscard]] bool isEnabled() const { return m_enabled; }

  void enqueueSentence(const QString &sentence, int speakerId = 0);

  void stopAndClear();

  int queueSize() const;

  void setVoice(const QString &voice);

  QString voice() const;

  QStringList availableVoices() const;

  void refreshVoices();

  [[nodiscard]] bool isSpeaking() const { return m_isPlaying; }

signals:
  void sentenceQueued(int speakerId);

  void sentenceFinished();

  void errorOccurred(const QString &error);

  void serverReady();

  void voiceChanged(const QString &voice);

  void voicesChanged(const QStringList &voices);

  void chunkReady(const AudioChunk &chunk);

  void chunkPlaybackStarted(const AudioChunk &chunk);

private slots:
  void playNextInQueue();

  void onAudioStateChanged(QAudio::State state);

private:
  void requestSynthesis(const QString &text, int speakerId,
                        quint64 generation);

  void processReply(QNetworkReply *reply, quint64 generation,
                    int speakerId);

  void finishCurrentPlayback();

  void cleanupAudioSink();

  static bool parseHeadTtsResponse(const QByteArray &payload,
                                   int speakerId,
                                   AudioChunk &out,
                                   QString &error);

  static QStringList defaultVoices();

private:
  QNetworkAccessManager *m_networkManager = nullptr;

  QUrl m_serverUrl;

  QNetworkReply *m_currentReply = nullptr;

  std::unique_ptr<QAudioSink> m_audioSink;

  QBuffer m_audioBuffer;

  QMutex m_audioBufferMutex;

  QQueue<AudioChunk> m_audioQueue;

  mutable QMutex m_queueMutex;

  bool m_enabled = false;
  bool m_initialized = false;

  bool m_isPlaying = false;
  bool m_playbackCompletionPending = false;

  quint64 m_generation = 0;

  QString m_voice = QStringLiteral("af_bella");

  QStringList m_availableVoices;

  static constexpr int DEFAULT_PORT = 8882;
};

#endif // TTSMANAGER_H