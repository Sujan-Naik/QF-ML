#include "../../include/voice/TtsManager.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>

TtsManager::TtsManager(QObject *parent)
    : QObject(parent), m_networkManager(new QNetworkAccessManager(this)),
      m_audioBuffer(this), m_dockerProcess(nullptr),
      m_healthCheckTimer(new QTimer(this)) {
  m_networkManager->setProxy(QNetworkProxy::NoProxy);

  m_availableVoices = defaultVoices();

  m_healthCheckTimer->setInterval(2000);

  connect(m_healthCheckTimer, &QTimer::timeout, this,
          &TtsManager::checkServerHealth);
}

TtsManager::~TtsManager() {
  stopAndClear();

  if (m_voiceReply) {
    m_voiceReply->abort();
    m_voiceReply->deleteLater();
    m_voiceReply = nullptr;
  }

  if (m_currentReply) {
    m_currentReply->abort();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
  }

  cleanupAudioSink();

  stopDockerContainer();
}

QStringList TtsManager::defaultVoices() {
  return QStringList{
      QStringLiteral("af_alloy"),    QStringLiteral("af_aoede"),
      QStringLiteral("af_bella"),    QStringLiteral("af_jessica"),
      QStringLiteral("af_kore"),     QStringLiteral("af_nicole"),
      QStringLiteral("af_nova"),     QStringLiteral("af_river"),
      QStringLiteral("af_sarah"),    QStringLiteral("af_sky"),

      QStringLiteral("am_adam"),     QStringLiteral("am_echo"),
      QStringLiteral("am_eric"),     QStringLiteral("am_fenrir"),
      QStringLiteral("am_liam"),     QStringLiteral("am_michael"),
      QStringLiteral("am_onyx"),     QStringLiteral("am_puck"),
      QStringLiteral("am_santa"),

      QStringLiteral("bf_alice"),    QStringLiteral("bf_emma"),
      QStringLiteral("bf_isabella"), QStringLiteral("bf_lily"),

      QStringLiteral("bm_daniel"),   QStringLiteral("bm_fable"),
      QStringLiteral("bm_george"),   QStringLiteral("bm_lewis")};
}

QStringList TtsManager::availableVoices() const { return m_availableVoices; }

void TtsManager::refreshVoices() {
  if (m_availableVoices.isEmpty())
    m_availableVoices = defaultVoices();

  emit voicesChanged(m_availableVoices);
}

bool TtsManager::initialize(const QString &serverUrl, bool autoStart) {
  if (m_initialized)
    return true;

  if (autoStart) {
    if (!checkDockerAvailable()) {
      emit errorOccurred(
          QStringLiteral("Docker is not available or user lacks permissions."));

      return false;
    }

    startDockerContainer();

    m_initialized = true;

    return true;
  }

  if (serverUrl.isEmpty())
    return false;

  m_serverUrl = QUrl(serverUrl);

  if (!m_serverUrl.isValid()) {
    qWarning() << "[TTS] Invalid server URL:" << serverUrl;

    return false;
  }

  m_initialized = true;

  return true;
}

void TtsManager::setEnabled(bool enabled) {
  if (m_enabled == enabled)
    return;

  m_enabled = enabled;

  qDebug() << "[TTS] setEnabled =" << enabled;

  if (!enabled)
    stopAndClear();
}

void TtsManager::setVoice(const QString &voice) {
  QString normalized = voice.trimmed();

  if (normalized.isEmpty())
    normalized = QStringLiteral("af_bella");

  if (m_voice == normalized)
    return;

  m_voice = normalized;

  qDebug() << "[TTS] Voice changed to:" << m_voice;

  emit voiceChanged(m_voice);
}

QString TtsManager::voice() const { return m_voice; }

void TtsManager::enqueueSentence(const QString &sentence, int speakerId) {
  const QString cleaned = sentence.trimmed();

  if (!m_enabled || !m_initialized || cleaned.isEmpty()) {
    return;
  }

  if (m_synthesisInProgress) {
    qWarning() << "[TTS] Synthesis already in progress;"
               << "ignoring overlapping sentence.";

    return;
  }

  m_synthesisInProgress = true;

  requestSynthesis(cleaned, speakerId, m_generation);
}

void TtsManager::requestSynthesis(const QString &text, int speakerId,
                                  quint64 generation) {
  if (!m_enabled || !m_initialized || generation != m_generation) {
    m_synthesisInProgress = false;
    return;
  }

  const QString url =
      m_serverUrl.toString() + QStringLiteral("/dev/captioned_speech");

  qDebug() << "[TTS] Synthesizing voice=" << m_voice
           << "text length=" << text.length() << "generation=" << generation;

  QJsonObject payload;

  payload["model"] = QStringLiteral("kokoro");
  payload["input"] = text;
  payload["voice"] = m_voice;
  payload["response_format"] = QStringLiteral("pcm");

  const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

  QNetworkRequest request{QUrl(url)};

  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));

  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("TalosApp/1.0"));

  QNetworkReply *reply = m_networkManager->post(request, body);

  m_currentReply = reply;

  connect(reply, &QNetworkReply::finished, this,
          [this, reply, generation, speakerId]() {
            processReply(reply, generation, speakerId);
          });
}

void TtsManager::processReply(QNetworkReply *reply, quint64 generation,
                              int speakerId) {
  if (!reply)
    return;

  if (m_currentReply == reply)
    m_currentReply = nullptr;

  if (generation != m_generation) {
    qDebug() << "[TTS] Ignoring stale synthesis reply."
             << "reply generation=" << generation
             << "current generation=" << m_generation;

    m_synthesisInProgress = false;

    reply->deleteLater();

    return;
  }

  if (!m_enabled) {
    m_synthesisInProgress = false;

    reply->deleteLater();

    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    const QString error = reply->errorString();

    m_synthesisInProgress = false;

    reply->deleteLater();

    emit errorOccurred(QStringLiteral("TTS request failed: %1").arg(error));

    return;
  }

  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  if (status != 200) {
    const QByteArray response = reply->readAll();

    QString error = QStringLiteral("TTS server returned HTTP %1").arg(status);

    if (!response.isEmpty()) {
      error += QStringLiteral(": ") + QString::fromUtf8(response);
    }

    m_synthesisInProgress = false;

    reply->deleteLater();

    emit errorOccurred(error);

    return;
  }

  const QByteArray payload = reply->readAll();

  if (payload.isEmpty()) {
    m_synthesisInProgress = false;

    reply->deleteLater();

    emit errorOccurred(QStringLiteral("Empty response from TTS server"));

    return;
  }

  AudioChunk chunk;
  QString parseError;

  if (!parseCaptionedResponse(payload, speakerId, chunk, parseError)) {
    m_synthesisInProgress = false;

    reply->deleteLater();

    emit errorOccurred(
        QStringLiteral("TTS response could not be parsed: %1").arg(parseError));

    return;
  }

  {
    QMutexLocker locker(&m_queueMutex);

    m_audioQueue.enqueue(chunk);
  }

  m_synthesisInProgress = false;

  emit sentenceQueued(speakerId);
  emit chunkReady(chunk);

  reply->deleteLater();

  if (!m_isPlaying)
    playNextInQueue();
}

bool TtsManager::parseCaptionedResponse(const QByteArray &payload,
                                        int speakerId,
                                        AudioChunk &out,
                                        QString &error) {
  QJsonParseError parseError;

  const QJsonDocument doc =
      QJsonDocument::fromJson(payload, &parseError);

  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    error = parseError.errorString();
    return false;
  }

  const QJsonObject object = doc.object();

  const QString audioB64 = object.value(QStringLiteral("audio")).toString();

  if (audioB64.isEmpty()) {
    error = QStringLiteral("missing 'audio' field");
    return false;
  }

  const QByteArray audio =
      QByteArray::fromBase64(audioB64.toUtf8());

  if (audio.isEmpty()) {
    error = QStringLiteral("audio decoded to zero bytes");
    return false;
  }

  out.data = audio;
  out.sampleRate = 24000;
  out.speakerId = speakerId;
  out.timestamp = QDateTime::currentMSecsSinceEpoch();
  out.visemes.clear();

  const QJsonArray timestamps =
      object.value(QStringLiteral("timestamps")).toArray();

  out.visemes.reserve(timestamps.size());

  for (const QJsonValue &value : timestamps) {
    if (!value.isObject()) {
      continue;
    }

    const QJsonObject entry = value.toObject();

    const QString word =
        entry.value(QStringLiteral("word")).toString().trimmed();

    if (word.isEmpty()) {
      continue;
    }

    const double startSeconds =
        entry.value(QStringLiteral("start_time")).toDouble();
    const double endSeconds =
        entry.value(QStringLiteral("end_time")).toDouble();

    Viseme viseme;
    viseme.startMs = static_cast<int>(startSeconds * 1000.0);
    viseme.endMs = static_cast<int>(endSeconds * 1000.0);
    viseme.shape = visemeForWord(word);

    out.visemes.append(viseme);
  }

  return true;
}

QString TtsManager::visemeForWord(const QString &word) {
  const QString lower = word.toLower();

  QString cleaned;
  cleaned.reserve(lower.size());

  for (QChar c : lower) {
    if (c.isLetter()) {
      cleaned.append(c);
    }
  }

  if (cleaned.isEmpty()) {
    return QStringLiteral("sil");
  }

  int a = 0, e = 0, i = 0, o = 0, u = 0;

  for (QChar c : cleaned) {
    if (c == QChar('a')) ++a;
    else if (c == QChar('e')) ++e;
    else if (c == QChar('i') || c == QChar('y')) ++i;
    else if (c == QChar('o')) ++o;
    else if (c == QChar('u')) ++u;
  }

  const int total = a + e + i + o + u;

  if (total == 0) {
    return QStringLiteral("M");
  }

  const int maxCount = std::max({a, e, i, o, u});

  if (a == maxCount) return QStringLiteral("A");
  if (e == maxCount) return QStringLiteral("E");
  if (i == maxCount) return QStringLiteral("I");
  if (o == maxCount) return QStringLiteral("O");
  return QStringLiteral("U");
}

void TtsManager::playNextInQueue() {
  if (!m_enabled)
    return;

  if (m_isPlaying)
    return;

  AudioChunk chunk;

  {
    QMutexLocker locker(&m_queueMutex);

    if (m_audioQueue.isEmpty())
      return;

    chunk = m_audioQueue.dequeue();
  }

  const QAudioDevice device = QMediaDevices::defaultAudioOutput();

  if (device.isNull()) {
    emit errorOccurred(QStringLiteral("No audio output device available."));

    return;
  }

  QAudioFormat format;

  format.setSampleRate(chunk.sampleRate);
  format.setChannelCount(1);
  format.setSampleFormat(QAudioFormat::Int16);

  cleanupAudioSink();

  {
    QMutexLocker locker(&m_audioBufferMutex);

    if (m_audioBuffer.isOpen())
      m_audioBuffer.close();

    m_audioBuffer.setData(chunk.data);

    if (!m_audioBuffer.open(QIODevice::ReadOnly)) {
      emit errorOccurred(QStringLiteral("Failed to open TTS audio buffer."));

      return;
    }
  }

  m_audioSink = std::make_unique<QAudioSink>(device, format);

  connect(m_audioSink.get(), &QAudioSink::stateChanged, this,
          &TtsManager::onAudioStateChanged, Qt::QueuedConnection);

  m_isPlaying = true;
  m_playbackCompletionPending = true;

  qDebug() << "[TTS] Playing chunk:" << chunk.data.size() << "bytes"
           << "visemes:" << chunk.visemes.size();

  m_audioSink->start(&m_audioBuffer);

  emit chunkPlaybackStarted(chunk);
}

void TtsManager::onAudioStateChanged(QAudio::State state) {
  qDebug() << "[TTS] Audio state changed:" << state;

  if (!m_audioSink)
    return;

  if (state == QAudio::IdleState) {
    if (!m_playbackCompletionPending)
      return;

    finishCurrentPlayback();

    return;
  }

  if (state == QAudio::StoppedState) {
    if (m_audioSink->error() != QAudio::NoError) {
      const QAudio::Error error = m_audioSink->error();

      qWarning() << "[TTS] Audio error:" << error;

      m_playbackCompletionPending = false;
      m_isPlaying = false;

      cleanupAudioSink();

      {
        QMutexLocker locker(&m_audioBufferMutex);

        if (m_audioBuffer.isOpen())
          m_audioBuffer.close();
      }

      emit errorOccurred(QStringLiteral("Audio playback error."));
    }
  }
}

void TtsManager::finishCurrentPlayback() {
  if (!m_playbackCompletionPending)
    return;

  m_playbackCompletionPending = false;

  if (m_audioSink)
    m_audioSink->stop();

  cleanupAudioSink();

  {
    QMutexLocker locker(&m_audioBufferMutex);

    if (m_audioBuffer.isOpen())
      m_audioBuffer.close();

    m_audioBuffer.setData(QByteArray());
  }

  m_isPlaying = false;

  emit sentenceFinished();
}

void TtsManager::cleanupAudioSink() {
  if (!m_audioSink)
    return;

  m_audioSink->disconnect();
  m_audioSink->stop();
  m_audioSink.reset();
}

void TtsManager::stopAndClear() {
  ++m_generation;

  qDebug() << "[TTS] stopAndClear:"
           << "generation=" << m_generation;

  m_synthesisInProgress = false;

  if (m_currentReply) {
    QNetworkReply *reply = m_currentReply;

    m_currentReply = nullptr;

    reply->abort();
    reply->deleteLater();
  }

  {
    QMutexLocker locker(&m_queueMutex);

    m_audioQueue.clear();
  }

  m_playbackCompletionPending = false;

  cleanupAudioSink();

  {
    QMutexLocker locker(&m_audioBufferMutex);

    if (m_audioBuffer.isOpen())
      m_audioBuffer.close();

    m_audioBuffer.setData(QByteArray());
  }

  m_isPlaying = false;
}

int TtsManager::queueSize() const {
  QMutexLocker locker(&m_queueMutex);

  return m_audioQueue.size();
}

bool TtsManager::checkDockerAvailable() {
  QProcess check;

  check.start(QStringLiteral("docker"), QStringList()
                                            << QStringLiteral("info"));

  check.waitForFinished(2000);

  if (check.exitCode() != 0) {
    qWarning() << "[TTS] Docker not available.";

    return false;
  }

  return true;
}

void TtsManager::startDockerContainer() {
  QProcess check;

  check.start(QStringLiteral("docker"),
              QStringList()
                  << QStringLiteral("ps") << QStringLiteral("--filter")
                  << (QStringLiteral("ancestor=") +
                      QString::fromUtf8(DOCKER_IMAGE))
                  << QStringLiteral("--format") << QStringLiteral("{{.ID}}"));

  check.waitForFinished(2000);

  const QString output =
      QString::fromUtf8(check.readAllStandardOutput()).trimmed();

  if (!output.isEmpty()) {
    m_containerId = output;
    m_containerStarted = true;

    m_serverUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(HOST_PORT));

    m_healthAttempts = 0;
    m_healthCheckTimer->start();

    return;
  }

  QProcess inspect;

  inspect.start(QStringLiteral("docker"),
                QStringList()
                    << QStringLiteral("image") << QStringLiteral("inspect")
                    << QString::fromUtf8(DOCKER_IMAGE));

  inspect.waitForFinished(2000);

  if (inspect.exitCode() != 0)
    pullDockerImage();
  else
    runDockerContainer();
}

void TtsManager::pullDockerImage() {
  if (m_dockerProcess)
    m_dockerProcess->deleteLater();

  m_dockerProcess = new QProcess(this);

  connect(m_dockerProcess, &QProcess::finished, this,
          &TtsManager::onDockerPullFinished);

  connect(m_dockerProcess, &QProcess::readyReadStandardOutput, this,
          &TtsManager::onDockerOutputReady);

  connect(m_dockerProcess, &QProcess::readyReadStandardError, this,
          &TtsManager::onDockerErrorReady);

  m_dockerProcess->start(QStringLiteral("docker"),
                         QStringList() << QStringLiteral("pull")
                                       << QString::fromUtf8(DOCKER_IMAGE));
}

void TtsManager::onDockerPullFinished(int exitCode,
                                      QProcess::ExitStatus status) {
  if (exitCode != 0 || status != QProcess::NormalExit) {
    emit errorOccurred(QStringLiteral("Failed to pull Docker image."));

    return;
  }

  runDockerContainer();
}

void TtsManager::runDockerContainer() {
  if (m_dockerProcess)
    m_dockerProcess->deleteLater();

  m_dockerProcess = new QProcess(this);

  connect(m_dockerProcess, &QProcess::finished, this,
          &TtsManager::onDockerRunFinished);

  connect(m_dockerProcess, &QProcess::readyReadStandardOutput, this,
          &TtsManager::onDockerOutputReady);

  connect(m_dockerProcess, &QProcess::readyReadStandardError, this,
          &TtsManager::onDockerErrorReady);

  QStringList args;

  args << QStringLiteral("run") << QStringLiteral("--rm")
       << QStringLiteral("-d") << QStringLiteral("--device=/dev/kfd")
       << QStringLiteral("--device=/dev/dri") << QStringLiteral("-e")
       << (QStringLiteral("HSA_OVERRIDE_GFX_VERSION=") +
           QString::fromUtf8(GFX_VERSION))
       << QStringLiteral("-p")
       << QStringLiteral("%1:%2").arg(HOST_PORT).arg(CONTAINER_PORT)
       << QString::fromUtf8(DOCKER_IMAGE);

  m_dockerProcess->start(QStringLiteral("docker"), args);

  m_containerStarted = true;
}

void TtsManager::onDockerRunFinished(int exitCode,
                                     QProcess::ExitStatus status) {
  if (exitCode != 0 || status != QProcess::NormalExit) {
    emit errorOccurred(QStringLiteral("Failed to start Docker container."));

    return;
  }

  if (m_containerId.isEmpty()) {
    const QString output =
        QString::fromUtf8(m_dockerProcess->readAllStandardOutput()).trimmed();

    if (!output.isEmpty())
      m_containerId = output;
  }

  m_serverUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(HOST_PORT));

  m_healthAttempts = 0;
  m_healthCheckTimer->start();
}

void TtsManager::onDockerOutputReady() {
  if (!m_dockerProcess)
    return;

  const QByteArray data = m_dockerProcess->readAllStandardOutput();

  if (!data.isEmpty())
    qDebug() << "[Docker stdout]" << data;
}

void TtsManager::onDockerErrorReady() {
  if (!m_dockerProcess)
    return;

  const QByteArray data = m_dockerProcess->readAllStandardError();

  if (!data.isEmpty())
    qWarning() << "[Docker stderr]" << data;
}

void TtsManager::checkServerHealth() {
  if (m_serverUrl.isEmpty()) {
    m_serverUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(HOST_PORT));
  }

  const QUrl healthUrl = m_serverUrl.resolved(QUrl(QStringLiteral("/health")));

  QNetworkRequest request{healthUrl};

  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("TalosApp/1.0"));

  QNetworkReply *reply = m_networkManager->get(request);

  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (reply->error() == QNetworkReply::NoError) {
      const int status =
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

      if (status == 200) {
        m_healthCheckTimer->stop();

        emit serverReady();

        reply->deleteLater();

        return;
      }
    }

    ++m_healthAttempts;

    if (m_healthAttempts >= MAX_HEALTH_ATTEMPTS) {
      m_healthCheckTimer->stop();

      emit errorOccurred(
          QStringLiteral("TTS server did not become ready within timeout."));
    }

    reply->deleteLater();
  });
}

void TtsManager::stopDockerContainer() {
  if (!m_containerStarted || m_containerId.isEmpty()) {
    return;
  }

  QProcess stop;

  stop.start(QStringLiteral("docker"),
             QStringList() << QStringLiteral("stop") << m_containerId);

  stop.waitForFinished(3000);

  m_containerStarted = false;
  m_containerId.clear();
}