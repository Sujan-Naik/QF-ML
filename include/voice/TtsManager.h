#ifndef TTSMANAGER_H
#define TTSMANAGER_H

#include "VisemeMap.h"

#include <QAudioSink>
#include <QBuffer>
#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QWebSocket>

#include <memory>

// TtsManager talks to a HeadTTS server over WebSocket. HeadTTS runs
// the timestamped Kokoro ONNX model and returns audio, Oculus visemes,
// and per-viseme timing.
//
// The WebSocket API is used rather than REST because the REST path on
// the HeadTTS Node server produces malformed audio on this platform.
// The browser demo, which uses WebSocket, produces correct audio. The
// two paths are handled by different code in headtts-node.mjs.
//
// Protocol:
//
//   1. On connect, send one "setup" message with voice, language,
//      speed, audioEncoding. The server is stateful per socket, so
//      this is re-sent only when a setting changes.
//   2. For each utterance, send a "synthesize" message with a unique
//      id and the input text.
//   3. The server replies with one text message of type "audio"
//      carrying words, visemes, vtimes, vdurations, and a "ref"
//      field matching the request id.
//   4. Immediately after, the server sends one binary message with
//      the raw PCM samples (or WAV, depending on audioEncoding).
//
//   Binary messages are correlated to their audio metadata by order:
//   the binary always follows the audio text message for the same
//   request. Interleaving only happens if more than one synthesis is
//   in flight, which TtsManager prevents.
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
  void onSocketConnected();

  void onSocketDisconnected();

  void onSocketError(QAbstractSocket::SocketError error);

  void onTextMessageReceived(const QString &message);

  void onBinaryMessageReceived(const QByteArray &data);

  void playNextInQueue();

  void onAudioStateChanged(QAudio::State state);

  void onProcessFinished(int exitCode, QProcess::ExitStatus status);

  void onProcessError(QProcess::ProcessError error);

private:
  void sendSetupMessage();

  void requestSynthesis(const QString &text, int speakerId);

  void finishCurrentPlayback();

  void cleanupAudioSink();

  void startHeadTts();
  void stopHeadTts();

  static QStringList defaultVoices();

  static QString resolveNodeBinary();
  static QString resolveHeadTtsEntry();
  static QString resolveHeadTtsWorkingDir();

private:
  QWebSocket m_socket;

  // Outstanding request bookkeeping. Only one synthesis is in flight
  // at a time, so a single set of fields is enough.
  bool m_synthesisInFlight = false;
  int m_currentRequestId = 0;
  int m_currentSpeakerId = 0;
  AudioChunk m_pendingChunk;
  bool m_awaitingBinary = false;

  // Setup state. Re-sent whenever it changes.
  bool m_setupSent = false;
  QString m_sentVoice;
  QString m_sentLanguage;
  QString m_sentAudioEncoding;

  std::unique_ptr<QAudioSink> m_audioSink;

  QBuffer m_audioBuffer;

  QMutex m_audioBufferMutex;

  QQueue<AudioChunk> m_audioQueue;

  mutable QMutex m_queueMutex;

  bool m_enabled = false;
  bool m_initialized = false;

  bool m_isPlaying = false;
  bool m_playbackCompletionPending = false;

  QString m_voice = QStringLiteral("af_bella");

  QStringList m_availableVoices;

  QProcess *m_headTtsProcess = nullptr;
  bool m_ownsProcess = false;

  static constexpr int DEFAULT_PORT = 8882;
};

#endif // TTSMANAGER_H