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


namespace {

// Linear-interpolation resampler for mono Int16 PCM. Sufficient for
// speech. Used to match the TTS output rate to the audio device's
// preferred sink rate.
QByteArray resampleLinear(const QByteArray &input, int srcRate,
                          int dstRate) {
  if (srcRate <= 0 || dstRate <= 0 || srcRate == dstRate) {
    return input;
  }

  const int srcSamples = input.size() / static_cast<int>(sizeof(int16_t));

  if (srcSamples <= 0) {
    return input;
  }

  const auto *src = reinterpret_cast<const int16_t *>(input.constData());

  const double ratio = static_cast<double>(dstRate) / srcRate;

  const int dstSamples = qMax(1, static_cast<int>(srcSamples * ratio));

  QByteArray out(dstSamples * static_cast<int>(sizeof(int16_t)),
                 Qt::Uninitialized);

  auto *dst = reinterpret_cast<int16_t *>(out.data());

  for (int i = 0; i < dstSamples; ++i) {
    const double pos = i / ratio;
    const int i0 = static_cast<int>(pos);
    const int i1 = qMin(i0 + 1, srcSamples - 1);
    const double frac = pos - i0;

    const double v = src[i0] * (1.0 - frac) + src[i1] * frac;

    dst[i] = static_cast<int16_t>(qBound(-32768.0, v, 32767.0));
  }

  return out;
}

} // namespace

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
}

namespace {

// Pitch-shift mono Int16 PCM by `semitones` (positive is higher),
// preserving duration and sample rate. Uses FFmpeg's asetrate to
// declare a shifted rate, then aresample to bring the rate back.
// Returns the original data unchanged on any failure.
QByteArray pitchShift(const QByteArray &input, int sampleRate,
                      double semitones) {
  if (qFuzzyIsNull(semitones) || input.isEmpty() || sampleRate <= 0) {
    return input;
  }

  // Clamp to one octave either way; beyond that the artefact is worse
  // than the effect.
  semitones = qBound(-12.0, semitones, 12.0);

  const double ratio = std::pow(2.0, semitones / 12.0);
  const int shiftedRate = qRound(sampleRate * ratio);

  // Build the filter graph. asetrate changes the declared rate, which
  // raises pitch; aresample returns to the original rate, which
  // restores duration.
  const QString filterDesc =
      QStringLiteral("asetrate=%1,aresample=%2")
          .arg(shiftedRate)
          .arg(sampleRate);

  AVFilterGraph *graph = avfilter_graph_alloc();
  if (!graph) {
    return input;
  }

  const AVFilter *buffersrc = avfilter_get_by_name("abuffer");
  const AVFilter *buffersink = avfilter_get_by_name("abuffersink");
  if (!buffersrc || !buffersink) {
    avfilter_graph_free(&graph);
    return input;
  }

  // abuffer input descriptor. fltp is required by the filter chain.
  const QString srcArgs = QStringLiteral(
      "time_base=1/%1:sample_rate=%1:sample_fmt=fltp:channel_layout=mono")
      .arg(sampleRate);

  AVFilterContext *srcCtx = nullptr;
  AVFilterContext *sinkCtx = nullptr;

  if (avfilter_graph_create_filter(&srcCtx, buffersrc, "in",
                                   srcArgs.toUtf8().constData(),
                                   nullptr, graph) < 0) {
    avfilter_graph_free(&graph);
    return input;
  }

  if (avfilter_graph_create_filter(&sinkCtx, buffersink, "out",
                                   nullptr, nullptr, graph) < 0) {
    avfilter_graph_free(&graph);
    return input;
  }

  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs = avfilter_inout_alloc();

  outputs->name = av_strdup("in");
  outputs->filter_ctx = srcCtx;
  outputs->pad_idx = 0;
  outputs->next = nullptr;

  inputs->name = av_strdup("out");
  inputs->filter_ctx = sinkCtx;
  inputs->pad_idx = 0;
  inputs->next = nullptr;

  if (avfilter_graph_parse_ptr(graph, filterDesc.toUtf8().constData(),
                               &inputs, &outputs, nullptr) < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    return input;
  }

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  if (avfilter_graph_config(graph, nullptr) < 0) {
    avfilter_graph_free(&graph);
    return input;
  }

  // Convert the Int16 input to a float planar frame.
  const int srcSamples = input.size() / static_cast<int>(sizeof(int16_t));
  const auto *src = reinterpret_cast<const int16_t *>(input.constData());

  AVFrame *frame = av_frame_alloc();
  frame->format = AV_SAMPLE_FMT_FLTP;
  frame->channel_layout = AV_CH_LAYOUT_MONO;
  frame->sample_rate = sampleRate;
  frame->nb_samples = srcSamples;

  if (av_frame_get_buffer(frame, 0) < 0) {
    av_frame_free(&frame);
    avfilter_graph_free(&graph);
    return input;
  }

  auto *dst = reinterpret_cast<float *>(frame->data[0]);
  for (int i = 0; i < srcSamples; ++i) {
    dst[i] = static_cast<float>(src[i]) / 32768.0f;
  }

  if (av_buffersrc_add_frame_flags(srcCtx, frame,
                                   AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
    av_frame_free(&frame);
    avfilter_graph_free(&graph);
    return input;
  }

  av_frame_free(&frame);

  // Pull frames from the sink and append them.
  QByteArray out;

  while (true) {
    AVFrame *outFrame = av_frame_alloc();
    const int ret = av_buffersink_get_frame(sinkCtx, outFrame);

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      av_frame_free(&outFrame);
      break;
    }

    if (ret < 0) {
      av_frame_free(&outFrame);
      break;
    }

    const int n = outFrame->nb_samples;
    const auto *p = reinterpret_cast<const float *>(outFrame->data[0]);

    const int offset = out.size();
    out.resize(offset + n * static_cast<int>(sizeof(int16_t)));
    auto *q = reinterpret_cast<int16_t *>(out.data() + offset);

    for (int i = 0; i < n; ++i) {
      q[i] = static_cast<int16_t>(
          qBound(-1.0f, p[i], 1.0f) * 32767.0f);
    }

    av_frame_free(&outFrame);
  }

  avfilter_graph_free(&graph);

  return out.isEmpty() ? input : out;
}

} // namespace


TtsManager::TtsManager(QObject *parent)
    : QObject(parent), m_audioBuffer(this) {
  m_availableVoices = defaultVoices();

  connect(&m_socket, &QWebSocket::connected, this,
          &TtsManager::onSocketConnected);

  connect(&m_socket, &QWebSocket::disconnected, this,
          &TtsManager::onSocketDisconnected);

  connect(&m_socket, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error),
        this, &TtsManager::onSocketError);

  connect(&m_socket, &QWebSocket::textMessageReceived, this,
          &TtsManager::onTextMessageReceived);

  connect(&m_socket, &QWebSocket::binaryMessageReceived, this,
          &TtsManager::onBinaryMessageReceived);
}

TtsManager::~TtsManager() {
  stopAndClear();
  stopHeadTts();

  m_socket.close();

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
    qWarning() << "[TTS] node binary not found.";
    emit errorOccurred(
        QStringLiteral("node binary not found. Set QF_NODE_CLI."));
    return;
  }

  if (entry.isEmpty() || workDir.isEmpty()) {
    qWarning() << "[TTS] HeadTTS entry script not found.";
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
}

void TtsManager::onProcessError(QProcess::ProcessError error) {
  qWarning() << "[TTS] HeadTTS process error:" << error;
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

  QUrl url;

  if (serverUrl.isEmpty()) {
    url = QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(DEFAULT_PORT));
  } else {
    url = QUrl(serverUrl);
  }

  if (!url.isValid()) {
    qWarning() << "[TTS] Invalid server URL:" << serverUrl;
    return false;
  }

  m_initialized = true;

  qDebug() << "[TTS] Using HeadTTS server:" << url.toString()
           << "autoStart=" << autoStart;

  if (autoStart) {
    startHeadTts();
  }

  m_socket.open(url);

  return true;
}

void TtsManager::onSocketConnected() {
  qDebug() << "[TTS] WebSocket connected.";

  m_setupSent = false;

  emit serverReady();
}

void TtsManager::onSocketDisconnected() {
  qDebug() << "[TTS] WebSocket disconnected.";

  m_setupSent = false;
  m_synthesisInFlight = false;
  m_awaitingBinary = false;
}

void TtsManager::onSocketError(QAbstractSocket::SocketError error) {
  qWarning() << "[TTS] WebSocket error:" << error;

  emit errorOccurred(
      QStringLiteral("HeadTTS WebSocket error: %1").arg(error));
}

void TtsManager::sendSetupMessage() {
  const QString language = QStringLiteral("en-us");
  const QString audioEncoding = QStringLiteral("pcm");

  if (m_setupSent && m_sentVoice == m_voice &&
      m_sentLanguage == language &&
      m_sentAudioEncoding == audioEncoding) {
    return;
  }

  QJsonObject data;
  data["voice"] = m_voice;
  data["language"] = language;
  data["speed"] = 1;
  data["audioEncoding"] = audioEncoding;

  QJsonObject message;
  message["type"] = QStringLiteral("setup");
  message["id"] = 1;
  message["data"] = data;

  const QByteArray body =
      QJsonDocument(message).toJson(QJsonDocument::Compact);

  m_socket.sendTextMessage(QString::fromUtf8(body));

  m_setupSent = true;
  m_sentVoice = m_voice;
  m_sentLanguage = language;
  m_sentAudioEncoding = audioEncoding;

  qDebug() << "[TTS] Setup sent: voice=" << m_voice;
}

void TtsManager::enqueueSentence(const QString &sentence, int speakerId) {
  const QString cleaned = sentence.trimmed();

  if (!m_enabled || !m_initialized || cleaned.isEmpty()) {
    return;
  }

  requestSynthesis(cleaned, speakerId);
}

void TtsManager::requestSynthesis(const QString &text, int speakerId) {
  if (m_synthesisInFlight) {
    qWarning() << "[TTS] Synthesis already in flight;"
               << "ignoring overlapping sentence.";
    return;
  }

  if (m_socket.state() != QAbstractSocket::ConnectedState) {
    qWarning() << "[TTS] Socket not connected; dropping sentence.";
    emit errorOccurred(QStringLiteral("HeadTTS socket is not connected."));
    return;
  }

  sendSetupMessage();

  m_currentRequestId = (m_currentRequestId % 100000) + 1;
  m_currentSpeakerId = speakerId;
  m_synthesisInFlight = true;

  qDebug() << "[TTS] Synthesizing voice=" << m_voice
           << "text length=" << text.length()
           << "requestId=" << m_currentRequestId;

  QJsonObject data;
  data["input"] = text;

  QJsonObject message;
  message["type"] = QStringLiteral("synthesize");
  message["id"] = m_currentRequestId;
  message["data"] = data;

  const QByteArray body =
      QJsonDocument(message).toJson(QJsonDocument::Compact);

  m_socket.sendTextMessage(QString::fromUtf8(body));
}

void TtsManager::onTextMessageReceived(const QString &message) {
  const QJsonDocument doc =
      QJsonDocument::fromJson(message.toUtf8());

  if (!doc.isObject()) {
    qWarning() << "[TTS] Non-object text message from HeadTTS.";
    return;
  }

  const QJsonObject object = doc.object();

  const QString type = object.value(QStringLiteral("type")).toString();

  if (type == QStringLiteral("error")) {
    const QJsonObject data =
        object.value(QStringLiteral("data")).toObject();

    const QString error =
        data.value(QStringLiteral("error")).toString();

    qWarning() << "[TTS] HeadTTS error:" << error;

    m_synthesisInFlight = false;
    m_awaitingBinary = false;

    emit errorOccurred(error);

    return;
  }

  if (type != QStringLiteral("audio")) {
    return;
  }

  const int ref = object.value(QStringLiteral("ref")).toInt();

  if (ref != m_currentRequestId) {
    qDebug() << "[TTS] Ignoring audio for stale request" << ref;
    return;
  }

  const QJsonObject data =
      object.value(QStringLiteral("data")).toObject();

  m_pendingChunk = AudioChunk();
  m_pendingChunk.sampleRate = 24000;
  m_pendingChunk.speakerId = m_currentSpeakerId;
  m_pendingChunk.timestamp = QDateTime::currentMSecsSinceEpoch();

  const QJsonArray visemeArray =
      data.value(QStringLiteral("visemes")).toArray();
  const QJsonArray vtimeArray =
      data.value(QStringLiteral("vtimes")).toArray();
  const QJsonArray vdurationArray =
      data.value(QStringLiteral("vdurations")).toArray();

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

  m_pendingChunk.visemes =
      VisemeMap::buildTimeline(visemes, vtimes, vdurations);

  m_awaitingBinary = true;
}

void TtsManager::onBinaryMessageReceived(const QByteArray &data) {
  if (!m_awaitingBinary) {
    qDebug() << "[TTS] Unexpected binary message; ignoring.";
    return;
  }

  m_awaitingBinary = false;
  m_synthesisInFlight = false;

  if (data.isEmpty()) {
    qWarning() << "[TTS] Empty binary audio message.";
    return;
  }

  m_pendingChunk.data = data;

  qDebug() << "[TTS] Received audio:"
           << data.size() << "bytes,"
           << m_pendingChunk.visemes.size() << "visemes";

  AudioChunk chunk = m_pendingChunk;
  m_pendingChunk = AudioChunk();

  {
    QMutexLocker locker(&m_queueMutex);
    m_audioQueue.enqueue(chunk);
  }

  emit sentenceQueued(m_currentSpeakerId);
  emit chunkReady(chunk);

  if (!m_isPlaying)
    playNextInQueue();
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
  m_setupSent = false;

  qDebug() << "[TTS] Voice changed to:" << m_voice;

  emit voiceChanged(m_voice);
}

QString TtsManager::voice() const { return m_voice; }

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

  const QAudioFormat preferred = device.preferredFormat();

  int sinkRate = preferred.sampleRate();

  if (sinkRate <= 0) {
    sinkRate = chunk.sampleRate;
  }

  QAudioFormat actual;
  actual.setSampleRate(sinkRate);
  actual.setChannelCount(1);
  actual.setSampleFormat(QAudioFormat::Int16);

  if (!device.isFormatSupported(actual)) {
    const QAudioFormat fallback = device.preferredFormat();

    actual = fallback;
    actual.setChannelCount(1);
    actual.setSampleFormat(QAudioFormat::Int16);

    qWarning() << "[TTS] Mono Int16 at" << sinkRate
               << "Hz is not supported; using"
               << actual.sampleRate() << "Hz";
  }

  // Pitch shift if configured. The chunk rate is unchanged by this
  // operation; only the frequency content moves.
  chunk.data = pitchShift(chunk.data, chunk.sampleRate, 4.0);

  // Resample the chunk to the sink rate if they differ. Without this,
  // a 24 kHz chunk played on a 48 kHz sink runs at double speed.
  if (actual.sampleRate() != chunk.sampleRate) {
    qDebug() << "[TTS] Resampling from" << chunk.sampleRate
             << "Hz to" << actual.sampleRate() << "Hz.";

    chunk.data = resampleLinear(chunk.data, chunk.sampleRate,
                                actual.sampleRate());
    chunk.sampleRate = actual.sampleRate();
  }

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

  m_audioSink = std::make_unique<QAudioSink>(device, actual);

  connect(m_audioSink.get(), &QAudioSink::stateChanged, this,
          &TtsManager::onAudioStateChanged, Qt::QueuedConnection);

  m_isPlaying = true;
  m_playbackCompletionPending = true;

  qDebug() << "[TTS] Playing chunk:" << chunk.data.size() << "bytes"
           << "rate:" << actual.sampleRate()
           << "visemes:" << chunk.visemes.size();

  m_audioSink->start(&m_audioBuffer);

  emit chunkPlaybackStarted(chunk);
}
void TtsManager::onAudioStateChanged(QAudio::State state) {
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
      qWarning() << "[TTS] Audio error:" << m_audioSink->error();

      m_playbackCompletionPending = false;
      m_isPlaying = false;

      cleanupAudioSink();

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
  qDebug() << "[TTS] stopAndClear";

  m_synthesisInFlight = false;
  m_awaitingBinary = false;
  m_pendingChunk = AudioChunk();

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