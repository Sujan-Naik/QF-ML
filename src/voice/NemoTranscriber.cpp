#include "../../include/voice/NemoTranscriber.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QThread>
#include <QTcpSocket>

#include <QCoreApplication>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr quint16 HTTP_PORT = 8080;
constexpr qint64 MAX_UPLOAD_SIZE = 2LL * 1024LL * 1024LL * 1024LL;
constexpr int MAX_HEADER_SIZE = 64 * 1024;

constexpr int kStreamingSampleRate = 16000;
constexpr int kPollIntervalMs = 10;

QByteArray jsonResponse(int statusCode, const QByteArray &reason,
                        const QJsonObject &object) {
  const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QByteArray response;
  response += "HTTP/1.1 ";
  response += QByteArray::number(statusCode);
  response += " ";
  response += reason;
  response += "\r\n";
  response += "Content-Type: application/json\r\n";
  response += "Content-Length: ";
  response += QByteArray::number(body.size());
  response += "\r\n";
  response += "Connection: close\r\n";
  response += "\r\n";
  response += body;
  return response;
}

void sendError(QTcpSocket *socket, int statusCode, const QByteArray &reason,
               const QString &message) {
  if (!socket) return;
  const QJsonObject object{{QStringLiteral("error"), message}};
  socket->write(jsonResponse(statusCode, reason, object));
  socket->disconnectFromHost();
}

} // namespace

// ---------------------------------------------------------------------------
// NemoStreamWorker
// ---------------------------------------------------------------------------

NemoStreamWorker::NemoStreamWorker(nemo_speech_asr_recognizer *recognizer,
                                   int32_t rnntRightContext, QObject *parent)
    : QObject(parent), m_recognizer(recognizer),
      m_rnntRightContext(rnntRightContext) {}

NemoStreamWorker::~NemoStreamWorker() {
  requestStop();
  if (m_stream) {
    nemo_speech_asr_stream_close(m_stream);
    m_stream = nullptr;
  }
}

void NemoStreamWorker::enqueueAudio(const std::vector<float> &pcm) {
  if (pcm.empty()) return;
  std::lock_guard<std::mutex> lock(m_mutex);
  m_pendingAudio.insert(m_pendingAudio.end(), pcm.begin(), pcm.end());
}

void NemoStreamWorker::requestStop() { m_stopRequested.store(true); }

bool NemoStreamWorker::isRunning() const { return m_running.load(); }

void NemoStreamWorker::run() {
  m_running.store(true);

  if (!m_recognizer) {
    emit streamFailed(QStringLiteral("Recognizer is not loaded."));
    m_running.store(false);
    return;
  }

  nemo_speech_asr_recognition_options options =
      nemo_speech_asr_recognition_options_default();
  options.enable_automatic_punctuation = true;
  options.interim_results = true;

  const nemo_speech_asr_status openStatus =
      nemo_speech_asr_streaming_recognize(m_recognizer, &options, &m_stream);

  if (openStatus != NEMO_SPEECH_ASR_OK || !m_stream) {
    const char *error = nemo_speech_asr_last_error();
    const QString errorText =
        error && *error
            ? QString::fromUtf8(error)
            : QStringLiteral("Failed to open streaming session.");
    qWarning() << "[NemoStreamWorker] Stream open failed:" << errorText;
    emit streamFailed(errorText);
    m_running.store(false);
    return;
  }

  emit streamOpened();

  while (!m_stopRequested.load()) {
    std::vector<float> audio;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_pendingAudio.empty()) {
        audio.swap(m_pendingAudio);
      }
    }

    if (!audio.empty()) {
      nemo_speech_asr_stream_push_f32(m_stream, audio.data(), audio.size(),
                                      kStreamingSampleRate);
    }

    processAvailableResults();

    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPollIntervalMs));
  }

  nemo_speech_asr_stream_finish(m_stream);
  processAvailableResults();

  nemo_speech_asr_stream_close(m_stream);
  m_stream = nullptr;

  emit streamClosed();
  m_running.store(false);
}

void NemoStreamWorker::processAvailableResults() {
  while (true) {
    nemo_speech_asr_result *result = nullptr;

    const nemo_speech_asr_status status =
        nemo_speech_asr_stream_next(m_stream, &result);

    if (status != NEMO_SPEECH_ASR_OK) return;
    if (!result) return;

    const bool isFinal = nemo_speech_asr_result_is_final(result);
    const char *text = nemo_speech_asr_result_transcript(result, 0);
    const QString transcript =
        text ? QString::fromUtf8(text).trimmed() : QString{};

    nemo_speech_asr_result_destroy(result);

    if (!transcript.isEmpty()) {
      emit segmentResult(transcript, isFinal);
    }
  }
}

// ---------------------------------------------------------------------------
// NemoTranscriber
// ---------------------------------------------------------------------------

NemoTranscriber::NemoTranscriber(const QString &modelPath, int gpu,
                                 const QString &language, QObject *parent)
    : QObject(parent), m_modelPath(modelPath), m_language(language.trimmed()) {
  const QByteArray path = modelPath.toUtf8();

  nemo_speech_asr_backend_config backend = {};
  backend.size = sizeof(backend);
  backend.gpu = gpu;

  nemo_speech_asr_model_config model = {};
  model.size = sizeof(model);
  model.path = path.constData();

  m_streamingConfig.size = sizeof(m_streamingConfig);
  m_streamingConfig.chunk_size = 0.16f;
  m_streamingConfig.rnnt_right_context = 1;

  nemo_speech_asr_recognizer_config config = {};
  config.size = sizeof(config);
  config.backend = &backend;
  config.model = &model;
  config.streaming = &m_streamingConfig;

  const nemo_speech_asr_status status =
      nemo_speech_asr_create(&config, &m_recognizer);

  if (status != NEMO_SPEECH_ASR_OK || !m_recognizer) {
    const char *error = nemo_speech_asr_last_error();
    const QString errorText =
        error && *error
            ? QString::fromUtf8(error)
            : QStringLiteral("Failed to create NeMo-Speech ASR recognizer.");
    qWarning() << "[NemoTranscriber] Failed to load model:" << modelPath
               << errorText;
    m_recognizer = nullptr;
    return;
  }

  qDebug() << "[NemoTranscriber] Loaded model:" << modelPath << "GPU:" << gpu;

  connect(&m_httpServer, &QTcpServer::newConnection, this, [this]() {
    while (m_httpServer.hasPendingConnections()) {
      QTcpSocket *socket = m_httpServer.nextPendingConnection();
      if (!socket) continue;

      socket->setProperty("headerComplete", false);
      socket->setProperty("contentLength", QVariant::fromValue<qlonglong>(-1));
      socket->setProperty("bytesReceived", QVariant::fromValue<qlonglong>(0));
      socket->setProperty("processing", false);

      const QString inputPath =
          QDir(QDir::tempPath())
              .filePath(QStringLiteral("qf-stt-%1-%2.mp4")
                            .arg(QString::number(QCoreApplication::applicationPid()))
                            .arg(QString::number(reinterpret_cast<quintptr>(socket))));

      socket->setProperty("inputPath", inputPath);

      auto *file = new QFile(inputPath, socket);
      if (!file->open(QIODevice::WriteOnly)) {
        sendError(socket, 500, "Internal Server Error",
                  QStringLiteral("Failed to create temporary upload file."));
        continue;
      }
      socket->setProperty("uploadFile", QVariant::fromValue<void *>(file));

      connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (!socket || !socket->isOpen()) return;
        if (socket->property("processing").toBool()) return;

        QByteArray buffer = socket->property("headerBuffer").toByteArray();

        if (!socket->property("headerComplete").toBool()) {
          buffer += socket->readAll();
          if (buffer.size() > MAX_HEADER_SIZE) {
            sendError(socket, 431, "Request Header Fields Too Large",
                      QStringLiteral("HTTP headers are too large."));
            return;
          }
          const int headerEnd = buffer.indexOf("\r\n\r\n");
          if (headerEnd < 0) {
            socket->setProperty("headerBuffer", buffer);
            return;
          }

          const QByteArray headers = buffer.left(headerEnd);
          const QByteArray initialBody = buffer.mid(headerEnd + 4);
          socket->setProperty("headerComplete", true);

          const QList<QByteArray> lines = headers.split('\n');
          if (lines.isEmpty()) {
            sendError(socket, 400, "Bad Request", QStringLiteral("Invalid HTTP request."));
            return;
          }
          const QByteArray requestLine = lines.first().trimmed();
          if (!requestLine.startsWith("POST /transcribe ")) {
            sendError(socket, 404, "Not Found", QStringLiteral("Endpoint not found."));
            return;
          }

          qint64 contentLength = -1;
          for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "content-length") {
              bool ok = false;
              const qlonglong length = value.toLongLong(&ok);
              if (!ok || length < 0) {
                sendError(socket, 400, "Bad Request", QStringLiteral("Invalid Content-Length."));
                return;
              }
              contentLength = length;
            }
          }

          if (contentLength < 0) {
            sendError(socket, 411, "Length Required", QStringLiteral("Content-Length is required."));
            return;
          }
          if (contentLength == 0) {
            sendError(socket, 400, "Bad Request", QStringLiteral("Empty upload."));
            return;
          }
          if (contentLength > MAX_UPLOAD_SIZE) {
            sendError(socket, 413, "Payload Too Large", QStringLiteral("Upload exceeds limit."));
            return;
          }

          socket->setProperty("contentLength", QVariant::fromValue<qlonglong>(contentLength));

          QFile *uploadFile = static_cast<QFile *>(socket->property("uploadFile").value<void *>());
          if (!uploadFile) {
            sendError(socket, 500, "Internal Server Error", QStringLiteral("Upload file is unavailable."));
            return;
          }

          if (!initialBody.isEmpty()) {
            const qint64 written = uploadFile->write(initialBody);
            if (written != initialBody.size()) {
              sendError(socket, 500, "Internal Server Error", QStringLiteral("Failed to write upload."));
              return;
            }
            socket->setProperty("bytesReceived", QVariant::fromValue<qlonglong>(written));
          }

          socket->setProperty("headerBuffer", QByteArray());
        } else {
          const QByteArray data = socket->readAll();
          if (data.isEmpty()) return;

          QFile *uploadFile = static_cast<QFile *>(socket->property("uploadFile").value<void *>());
          if (!uploadFile) {
            sendError(socket, 500, "Internal Server Error", QStringLiteral("Upload file is unavailable."));
            return;
          }

          const qint64 previous = socket->property("bytesReceived").toLongLong();
          const qint64 contentLength = socket->property("contentLength").toLongLong();
          const qint64 remaining = contentLength - previous;
          const qint64 toWrite = std::min(remaining, static_cast<qint64>(data.size()));

          if (toWrite > 0) {
            const qint64 written = uploadFile->write(data.constData(), toWrite);
            if (written != toWrite) {
              sendError(socket, 500, "Internal Server Error", QStringLiteral("Failed to write upload."));
              return;
            }
            socket->setProperty("bytesReceived", QVariant::fromValue<qlonglong>(previous + written));
          }
        }

        const qint64 received = socket->property("bytesReceived").toLongLong();
        const qint64 contentLength = socket->property("contentLength").toLongLong();
        if (contentLength <= 0 || received < contentLength) return;

        socket->setProperty("processing", true);

        QFile *uploadFile = static_cast<QFile *>(socket->property("uploadFile").value<void *>());
        if (uploadFile) {
          uploadFile->flush();
          uploadFile->close();
        }

        const QString inputPath = socket->property("inputPath").toString();
        const QString pcmPath = inputPath + QStringLiteral(".pcm");

        QProcess ffmpeg;
        QStringList args;
        args << "-hide_banner" << "-loglevel" << "error" << "-y"
             << "-i" << inputPath << "-vn"
             << "-ar" << "16000" << "-ac" << "1"
             << "-f" << "f32le" << pcmPath;

        ffmpeg.start(QStringLiteral("ffmpeg"), args);
        if (!ffmpeg.waitForStarted(5000)) {
          QFile::remove(inputPath);
          if (uploadFile) uploadFile->deleteLater();
          sendError(socket, 500, "Internal Server Error", QStringLiteral("Failed to start FFmpeg."));
          return;
        }

        const bool finished = ffmpeg.waitForFinished(-1);
        if (!finished || ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
          const QString ffmpegError = QString::fromLocal8Bit(ffmpeg.readAllStandardError()).trimmed();
          QFile::remove(inputPath);
          QFile::remove(pcmPath);
          if (uploadFile) uploadFile->deleteLater();
          sendError(socket, 500, "Internal Server Error",
                    ffmpegError.isEmpty() ? QStringLiteral("FFmpeg failed.") : ffmpegError);
          return;
        }

        QFile::remove(inputPath);
        if (uploadFile) uploadFile->deleteLater();

        QFile pcmFile(pcmPath);
        if (!pcmFile.open(QIODevice::ReadOnly)) {
          QFile::remove(pcmPath);
          sendError(socket, 500, "Internal Server Error", QStringLiteral("Failed to open generated PCM."));
          return;
        }

        const qint64 pcmSize = pcmFile.size();
        if (pcmSize <= 0 || pcmSize % static_cast<qint64>(sizeof(float)) != 0) {
          pcmFile.close();
          QFile::remove(pcmPath);
          sendError(socket, 500, "Internal Server Error", QStringLiteral("Invalid generated PCM."));
          return;
        }

        std::vector<float> pcm32f;
        pcm32f.resize(static_cast<size_t>(pcmSize / sizeof(float)));

        const qint64 readBytes = pcmFile.read(reinterpret_cast<char *>(pcm32f.data()), pcmSize);
        pcmFile.close();
        QFile::remove(pcmPath);

        if (readBytes != pcmSize) {
          sendError(socket, 500, "Internal Server Error", QStringLiteral("Failed to read complete PCM."));
          return;
        }

        const QString text = transcribe(pcm32f);
        const QJsonObject json{{QStringLiteral("text"), text}};
        socket->write(jsonResponse(200, "OK", json));
        socket->disconnectFromHost();
      });

      connect(socket, &QTcpSocket::disconnected, this, [socket]() {
        const QString inputPath = socket->property("inputPath").toString();
        if (!inputPath.isEmpty()) QFile::remove(inputPath);
        const QString pcmPath = inputPath + QStringLiteral(".pcm");
        if (!pcmPath.isEmpty()) QFile::remove(pcmPath);

        QFile *file = static_cast<QFile *>(socket->property("uploadFile").value<void *>());
        if (file) {
          if (file->isOpen()) file->close();
          file->deleteLater();
        }
        socket->deleteLater();
      });
    }
  });

  if (!m_httpServer.listen(QHostAddress::LocalHost, HTTP_PORT)) {
    qWarning() << "[NemoTranscriber] Failed to start HTTP server:"
               << m_httpServer.errorString();
  } else {
    qDebug() << "[NemoTranscriber] Temporary STT HTTP endpoint:"
             << "http://127.0.0.1:" << HTTP_PORT << "/transcribe";
  }
}

NemoTranscriber::~NemoTranscriber() {
  stopStreaming();
  m_httpServer.close();
  if (m_recognizer) {
    nemo_speech_asr_destroy(m_recognizer);
    m_recognizer = nullptr;
  }
}

bool NemoTranscriber::isLoaded() const { return m_recognizer != nullptr; }

bool NemoTranscriber::startStreaming(int32_t rightContext) {
  if (!m_recognizer) {
    emit transcriptionError(QStringLiteral("Recognizer is not loaded."));
    return false;
  }
  if (m_worker) {
    return true;
  }

  m_workerThread = new QThread(this);
  m_worker = new NemoStreamWorker(m_recognizer, rightContext);
  m_worker->moveToThread(m_workerThread);

  connect(m_workerThread, &QThread::started, m_worker, &NemoStreamWorker::run);

  connect(m_worker, &NemoStreamWorker::streamOpened, this,
          &NemoTranscriber::streamOpened);

  connect(m_worker, &NemoStreamWorker::streamFailed, this,
          [this](const QString &error) {
            emit transcriptionError(error);
          });

  connect(m_worker, &NemoStreamWorker::segmentResult, this,
          &NemoTranscriber::liveSegment);

  connect(m_worker, &NemoStreamWorker::streamClosed, this,
          &NemoTranscriber::streamClosed);

  connect(m_worker, &NemoStreamWorker::streamClosed, this, [this]() {
    if (m_workerThread) {
      m_workerThread->quit();
    }
  });

  connect(m_workerThread, &QThread::finished, m_worker,
          &QObject::deleteLater);

  connect(m_workerThread, &QThread::finished, this, [this]() {
    m_workerThread->deleteLater();
    m_workerThread = nullptr;
    m_worker = nullptr;
  });

  m_workerThread->start();
  return true;
}

void NemoTranscriber::feedAudio(const std::vector<float> &pcm) {
  if (!m_worker) return;
  m_worker->enqueueAudio(pcm);
}

void NemoTranscriber::stopStreaming() {
  if (!m_worker) return;
  m_worker->requestStop();
}

bool NemoTranscriber::isStreaming() const {
  return m_workerThread != nullptr && m_workerThread->isRunning();
}

QString NemoTranscriber::transcribe(const std::vector<float> &pcm32f) {
  if (!m_recognizer) {
    const QString errorText =
        QStringLiteral("NeMo-Speech recognizer is not loaded.");
    qWarning() << "[NemoTranscriber]" << errorText;
    emit transcriptionError(errorText);
    return {};
  }

  if (pcm32f.empty()) return {};

  nemo_speech_asr_recognition_options options =
      nemo_speech_asr_recognition_options_default();
  options.enable_automatic_punctuation = true;

  constexpr int32_t sampleRate = 16000;
  nemo_speech_asr_result *result = nullptr;

  const nemo_speech_asr_status status =
      nemo_speech_asr_recognize_f32(m_recognizer, &options, pcm32f.data(),
                                    pcm32f.size(), sampleRate, &result);

  if (status != NEMO_SPEECH_ASR_OK || !result) {
    const char *error = nemo_speech_asr_last_error();
    const QString errorText =
        error && *error
            ? QString::fromUtf8(error)
            : QStringLiteral("Unknown NeMo-Speech transcription error.");
    qWarning() << "[NemoTranscriber] Transcription failed:" << errorText;
    emit transcriptionError(errorText);
    return {};
  }

  const char *transcript = nemo_speech_asr_result_transcript(result, 0);
  const QString text =
      transcript ? QString::fromUtf8(transcript).trimmed() : QString{};

  nemo_speech_asr_result_destroy(result);

  if (text.isEmpty()) return {};

  emit transcriptionFinished(text);
  return text;
}

QString NemoTranscriber::modelPath() const { return m_modelPath; }
QString NemoTranscriber::language() const { return m_language; }

void NemoTranscriber::setLanguage(const QString &language) {
  m_language = language.trimmed();
}