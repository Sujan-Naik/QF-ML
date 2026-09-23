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
#include <QProcess>
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
// The server is launched by absolute path: node is invoked with the
// HeadTTS entry module as its argument, with the HeadTTS directory as
// the working directory. npm is not involved, start.sh is not
// involved, and nothing in the launch path depends on the parent
// process's PATH. Both the node binary and the HeadTTS directory are
// baked into qf-inference by CMake at configure time.
//
// If the compile-time paths are wrong or missing, the environment
// variables QF_NODE_CLI and QF_HEADTTS_DIR can override them.
class TtsManager : public QObject {
  Q_OBJECT

public:
  explicit TtsManager(QObject *parent = nullptr);

  ~TtsManager() override;

  bool initialize(const QString &serverUrl = QString(),
                  bool autoStart = true);

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

  void onProcessFinished(int exitCode, QProcess::ExitStatus status);

  void onProcessError(QProcess::ProcessError error);

  void checkServerHealth();

private:
  void requestSynthesis(const QString &text, int speakerId,
                        quint64 generation);

  void processReply(QNetworkReply *reply, quint64 generation,
                    int speakerId);

  void finishCurrentPlayback();

  void cleanupAudioSink();

  void startHeadTts();
  void stopHeadTts();

  static bool parseHeadTtsResponse(const QByteArray &payload,
                                   int speakerId,
                                   AudioChunk &out,
                                   QString &error);

  static QStringList defaultVoices();

  static QString resolveNodeBinary();
  static QString resolveHeadTtsEntry();
  static QString resolveHeadTtsWorkingDir();

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

  QProcess *m_headTtsProcess = nullptr;
  QTimer *m_healthCheckTimer = nullptr;
  int m_healthAttempts = 0;
  bool m_ownsProcess = false;

  static constexpr int DEFAULT_PORT = 8882;
  static constexpr int HEALTH_CHECK_INTERVAL_MS = 500;
  static constexpr int MAX_HEALTH_ATTEMPTS = 120;
};

#endif // TTSMANAGER_H