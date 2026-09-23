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

class TtsManager : public QObject {
  Q_OBJECT

public:
  explicit TtsManager(QObject *parent = nullptr);

  ~TtsManager() override;

  bool initialize(const QString &serverUrl = QString(), bool autoStart = true);

  void setEnabled(bool enabled);

  [[nodiscard]] bool isEnabled() const { return m_enabled; }

  void enqueueSentence(const QString &sentence, int speakerId = 0);

  void stopAndClear();

  int queueSize() const;

  void setVoice(const QString &voice);

  QString voice() const;

  QStringList availableVoices() const;

  void refreshVoices();

  void stopDockerContainer();

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

  void onDockerPullFinished(int exitCode, QProcess::ExitStatus status);

  void onDockerRunFinished(int exitCode, QProcess::ExitStatus status);

  void onDockerOutputReady();

  void onDockerErrorReady();

  void checkServerHealth();

private:
  // The two-step synthesis flow. requestPhonemes fetches the IPA
  // phoneme sequence for the sentence; when it arrives, synthesize
  // posts to the captioned speech endpoint; when that arrives,
  // processReply builds the viseme timeline from the phonemes and the
  // word timestamps.
  void requestPhonemes(const QString &text, int speakerId,
                       quint64 generation);
  void synthesize(const QString &text, int speakerId, quint64 generation,
                  const QStringList &wordPhonemes);

  void processReply(QNetworkReply *reply, quint64 generation, int speakerId,
                    const QStringList &wordPhonemes);

  void finishCurrentPlayback();

  void cleanupAudioSink();

  bool checkDockerAvailable();

  void startDockerContainer();

  void pullDockerImage();

  void runDockerContainer();

  static bool parseCaptionedResponse(const QByteArray &payload,
                                     int speakerId,
                                     const QStringList &wordPhonemes,
                                     AudioChunk &out,
                                     QString &error);

  static QString visemeForWord(const QString &word);

  static QStringList defaultVoices();

private:
  QNetworkAccessManager *m_networkManager = nullptr;

  QUrl m_serverUrl;

  QNetworkReply *m_currentReply = nullptr;
  QNetworkReply *m_voiceReply = nullptr;
  QNetworkReply *m_phonemeReply = nullptr;

  std::unique_ptr<QAudioSink> m_audioSink;

  QBuffer m_audioBuffer;

  QMutex m_audioBufferMutex;

  QQueue<AudioChunk> m_audioQueue;

  mutable QMutex m_queueMutex;

  bool m_enabled = false;
  bool m_initialized = false;

  bool m_isPlaying = false;
  bool m_playbackCompletionPending = false;

  bool m_synthesisInProgress = false;

  quint64 m_generation = 0;

  QString m_voice = QStringLiteral("af_bella");

  QStringList m_availableVoices;

  QProcess *m_dockerProcess = nullptr;

  bool m_containerStarted = false;

  QString m_containerId;

  QTimer *m_healthCheckTimer = nullptr;

  int m_healthAttempts = 0;

  static constexpr const char *DOCKER_IMAGE =
      "ghcr.io/remsky/kokoro-fastapi-rocm:latest";

  static constexpr int HOST_PORT = 8880;
  static constexpr int CONTAINER_PORT = 8880;

  static constexpr const char *GFX_VERSION = "10.3.0";

  static constexpr int MAX_HEALTH_ATTEMPTS = 60;
};

#endif // TTSMANAGER_H