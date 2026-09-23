#include "../../include/voice/TtsManager.h"

#include "../../include/voice/VisemeMap.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QNetworkProxy>
#include <QNetworkRequest>

TtsManager::TtsManager(QObject *parent)
    : QObject(parent), m_networkManager(new QNetworkAccessManager(this)),
      m_audioBuffer(this), m_healthCheckTimer(new QTimer(this)) {
  m_networkManager->setProxy(QNetworkProxy::NoProxy);

  m_availableVoices = defaultVoices();

  m_healthCheckTimer->setInterval(HEALTH_CHECK_INTERVAL_MS);

  connect(m_healthCheckTimer, &QTimer::timeout, this,
          &TtsManager::checkServerHealth);
}

TtsManager::~TtsManager() {
  stopAndClear();
  stopHeadTts();

  if (m_currentReply) {
    m_currentReply->abort();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
  }

  cleanupAudioSink();
}

QString TtsManager::resolveNodeBinary() {
  const QString fromEnv =
      qEnvironmentVariable("QF_NODE_CLI").trimmed();

  if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv)) {
    return fromEnv;
  }

#ifdef QF_NODE_CLI_DEFAULT
  const QString fromBuild = QStringLiteral(QF_NODE_CLI_DEFAULT);

  if (!fromBuild.isEmpty() && QFileInfo::exists(fromBuild)) {
    return fromBuild;
  }
#endif

  // Development fallback. Search nvm's directory for the newest
  // installed node. This is a convenience, not the mechanism.
  const QString nvmRoot =
      QDir::homePath() + QStringLiteral("/.nvm/versions/node");

  QDir nvmDir(nvmRoot);

  const QStringList versions =
      nvmDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

  for (int i = versions.size() - 1; i >= 0; --i) {
    const QString candidate =
        nvmDir.filePath(versions.at(i) + QStringLiteral("/bin/node"));

    if (QFileInfo::exists(candidate)) {
      return candidate;
    }
  }

  return {};
}

QString TtsManager::resolveHeadTtsEntry() {
#ifdef QF_HEADTTS_DIR_DEFAULT
  const QString dir = QStringLiteral(QF_HEADTTS_DIR_DEFAULT);

  if (!dir.isEmpty()) {
    const QString entry =
        QDir(dir).filePath(QStringLiteral("modules/headtts-node.mjs"));

    if (QFileInfo::exists(entry)) {
      return entry;
    }
  }
#endif

  return {};
}

QString TtsManager::resolveHeadTtsWorkingDir() {
#ifdef QF_HEADTTS_DIR_DEFAULT
  const QString dir = QStringLiteral(QF_HEADTTS_DIR_DEFAULT);

  if (!dir.isEmpty() && QFileInfo::exists(dir)) {
    return dir;
  }
#endif

  return {};
}

void TtsManager::startHeadTts() {
  if (m_headTtsProcess) {
    return;
  }

  const QString node = resolveNodeBinary();
  const QString entry = resolveHeadTtsEntry();
  const QString workDir = resolveHeadTtsWorkingDir();

  if (node.isEmpty()) {
    qWarning() << "[TTS] node binary not found. Set QF_NODE_CLI or "
                  "ensure nvm has an installed Node.";
    emit errorOccurred(
        QStringLiteral("node binary not found. Set QF_NODE_CLI."));
    return;
  }

  if (entry.isEmpty() || workDir.isEmpty()) {
    qWarning() << "[TTS] HeadTTS entry script not found. The tree "
                  "was probably built without HeadTTS installed.";
    emit errorOccurred(
        QStringLiteral("HeadTTS entry script not found."));
    return;
  }

  qDebug() << "[TTS] Starting HeadTTS:";
  qDebug() << "[TTS]   node:" << node;
  qDebug() << "[TTS]   entry:" << entry;
  qDebug() << "[TTS]   cwd:" << workDir;

  m_headTtsProcess = new QProcess(this);

  m_headTtsProcess->setWorkingDirectory(workDir);

  // Inherit the parent's environment, but that is only a starting
  // point. The child does not need npm or node on PATH, because node
  // is invoked by absolute path and its own binary directory is
  // added explicitly so any subprocess node spawns can find its own
  // siblings.
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

  const QFileInfo nodeInfo(node);
  const QString nodeBinDir = nodeInfo.absolutePath();

  const QString existingPath = env.value(QStringLiteral("PATH"));

  env.insert(QStringLiteral("PATH"),
             nodeBinDir + QLatin1Char(':') + existingPath);

  m_headTtsProcess->setProcessEnvironment(env);

  m_headTtsProcess->setProcessChannelMode(QProcess::MergedChannels);

  connect(m_headTtsProcess, &QProcess::finished, this,
          &TtsManager::onProcessFinished);

  connect(m_headTtsProcess, &QProcess::errorOccurred, this,
          &TtsManager::onProcessError);

  connect(m_headTtsProcess, &QProcess::readyReadStandardOutput, this,
          [this]() {
            const QByteArray out = m_headTtsProcess->readAllStandardOutput();

            if (!out.isEmpty()) {
              qDebug() << "[HeadTTS]" << out.trimmed();
            }
          });

  m_headTtsProcess->start(node, QStringList{entry});

  m_ownsProcess = true;
}

void TtsManager::stopHeadTts() {
  if (!m_headTtsProcess) {
    return;
  }

  m_healthCheckTimer->stop();

  if (m_headTtsProcess->state() != QProcess::NotRunning) {
    qDebug() << "[TTS] Stopping HeadTTS.";

    m_headTtsProcess->terminate();

    if (!m_headTtsProcess->waitForFinished(5000)) {
      qWarning() << "[TTS] HeadTTS did not stop, killing.";
      m_headTtsProcess->kill();
      m_headTtsProcess->waitForFinished(2000);
    }
  }

  m_headTtsProcess->deleteLater();
  m_headTtsProcess = nullptr;
  m_ownsProcess = false;
}

void TtsManager::onProcessFinished(int exitCode,
                                   QProcess::ExitStatus status) {
  qWarning() << "[TTS] HeadTTS process finished. exitCode=" << exitCode
             << "status=" << status;

  if (m_headTtsProcess) {
    m_headTtsProcess->deleteLater();
    m_headTtsProcess = nullptr;
  }

  m_ownsProcess = false;

  emit errorOccurred(QStringLiteral("HeadTTS process exited unexpectedly."));
}

void TtsManager::onProcessError(QProcess::ProcessError error) {
  qWarning() << "[TTS] HeadTTS process error:" << error;

  emit errorOccurred(
      QStringLiteral("HeadTTS process error: %1").arg(error));
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

  if (serverUrl.isEmpty()) {
    m_serverUrl = QUrl(QStringLiteral("http://127.0.0.1:%1")
                           .arg(DEFAULT_PORT));
  } else {
    m_serverUrl = QUrl(serverUrl);
  }

  if (!m_serverUrl.isValid()) {
    qWarning() << "[TTS] Invalid server URL:" << serverUrl;
    return false;
  }

  m_initialized = true;

  qDebug() << "[TTS] Using HeadTTS server:" << m_serverUrl.toString()
           << "autoStart=" << autoStart;

  if (autoStart) {
    startHeadTts();
  }

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

  requestSynthesis(cleaned, speakerId, m_generation);
}

void TtsManager::requestSynthesis(const QString &text, int speakerId,
                                  quint64 generation) {
  if (!m_enabled || !m_initialized || generation != m_generation) {
    return;
  }

  const QString url =
      m_serverUrl.toString() + QStringLiteral("/v1/synthesize");

  qDebug() << "[TTS] Synthesizing voice=" << m_voice
           << "text length=" << text.length() << "generation=" << generation;

  QJsonObject payload;

  payload["input"] = text;
  payload["voice"] = m_voice;
  payload["language"] = QStringLiteral("en-us");
  payload["speed"] = 1.0;
  payload["audioEncoding"] = QStringLiteral("pcm");

  const QByteArray body =
      QJsonDocument(payload).toJson(QJsonDocument::Compact);

  QNetworkRequest request{QUrl(url)};

  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/json"));

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

    reply->deleteLater();

    return;
  }

  if (!m_enabled) {
    reply->deleteLater();

    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    const QString error = reply->errorString();

    reply->deleteLater();

    emit errorOccurred(QStringLiteral("TTS request failed: %1").arg(error));

    return;
  }

  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  if (status != 200) {
    const QByteArray response = reply->readAll();

    QString error = QStringLiteral("HeadTTS returned HTTP %1").arg(status);

    if (!response.isEmpty()) {
      error += QStringLiteral(": ") + QString::fromUtf8(response);
    }

    reply->deleteLater();

    emit errorOccurred(error);

    return;
  }

  const QByteArray payload = reply->readAll();

  if (payload.isEmpty()) {
    reply->deleteLater();

    emit errorOccurred(QStringLiteral("Empty response from HeadTTS"));

    return;
  }

  AudioChunk chunk;
  QString parseError;

  if (!parseHeadTtsResponse(payload, speakerId, chunk, parseError)) {
    reply->deleteLater();

    emit errorOccurred(
        QStringLiteral("HeadTTS response could not be parsed: %1")
            .arg(parseError));

    return;
  }

  {
    QMutexLocker locker(&m_queueMutex);

    m_audioQueue.enqueue(chunk);
  }

  emit sentenceQueued(speakerId);
  emit chunkReady(chunk);

  reply->deleteLater();

  if (!m_isPlaying)
    playNextInQueue();
}

bool TtsManager::parseHeadTtsResponse(const QByteArray &payload,
                                      int speakerId,
                                      AudioChunk &out,
                                      QString &error) {
  QJsonParseError parseError;

  const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);

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

  const QByteArray audio = QByteArray::fromBase64(audioB64.toUtf8());

  if (audio.isEmpty()) {
    error = QStringLiteral("audio decoded to zero bytes");
    return false;
  }

  out.data = audio;
  out.sampleRate = 24000;
  out.speakerId = speakerId;
  out.timestamp = QDateTime::currentMSecsSinceEpoch();

  const QJsonArray visemeArray =
      object.value(QStringLiteral("visemes")).toArray();
  const QJsonArray vtimeArray =
      object.value(QStringLiteral("vtimes")).toArray();
  const QJsonArray vdurationArray =
      object.value(QStringLiteral("vdurations")).toArray();

  const int count =
      qMin(visemeArray.size(), qMin(vtimeArray.size(), vdurationArray.size()));

  QStringList visemes;
  QVector<int> vtimes;
  QVector<int> vdurations;

  visemes.reserve(count);
  vtimes.reserve(count);
  vdurations.reserve(count);

  for (int i = 0; i < count; ++i) {
    visemes.append(visemeArray.at(i).toString());
    vtimes.append(vtimeArray.at(i).toInt());
    vdurations.append(vdurationArray.at(i).toInt());
  }

  out.visemes = VisemeMap::buildTimeline(visemes, vtimes, vdurations);

  return true;
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

void TtsManager::checkServerHealth() {
  const QUrl healthUrl =
      m_serverUrl.resolved(QUrl(QStringLiteral("/health")));

  QNetworkRequest request{healthUrl};

  QNetworkReply *reply = m_networkManager->get(request);

  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (reply->error() == QNetworkReply::NoError) {
      const int status =
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

      if (status == 200) {
        m_healthCheckTimer->stop();

        qDebug() << "[TTS] HeadTTS is ready.";

        emit serverReady();

        reply->deleteLater();

        return;
      }
    }

    ++m_healthAttempts;

    if (m_healthAttempts >= MAX_HEALTH_ATTEMPTS) {
      m_healthCheckTimer->stop();

      emit errorOccurred(
          QStringLiteral("HeadTTS did not become ready within timeout."));
    }

    reply->deleteLater();
  });
}