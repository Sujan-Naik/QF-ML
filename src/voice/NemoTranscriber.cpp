#include "../../include/voice/NemoTranscriber.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpSocket>

#include <QCoreApplication>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr quint16 HTTP_PORT = 8080;

// Temporary safety limit for uploaded files.
// Increase/remove if you need larger files.
constexpr qint64 MAX_UPLOAD_SIZE = 2LL * 1024LL * 1024LL * 1024LL;

// Maximum amount of HTTP header data we'll accept.
constexpr int MAX_HEADER_SIZE = 64 * 1024;

QString makeTempPath(const QString &suffix) {
  const QString tempDir = QDir::tempPath();

  const QString filename =
      QStringLiteral("qf-stt-%1%2")
          .arg(QString::number(QCoreApplication::applicationPid()))
          .arg(suffix);

  return QDir(tempDir).filePath(filename);
}

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
  if (!socket)
    return;

  const QJsonObject object{{QStringLiteral("error"), message}};

  socket->write(jsonResponse(statusCode, reason, object));

  socket->disconnectFromHost();
}

} // namespace

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

  nemo_speech_asr_recognizer_config config = {};
  config.size = sizeof(config);
  config.backend = &backend;
  config.model = &model;

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

  /*
   * Temporary local HTTP endpoint.
   *
   * Example:
   *
   * curl \
   *   -H "Content-Type: video/mp4" \
   *   --data-binary "@final battle.mp4" \
   *   http://127.0.0.1:8080/transcribe
   *
   * The request body is streamed directly to a temporary MP4
   * instead of being accumulated in RAM.
   */

  connect(&m_httpServer, &QTcpServer::newConnection, this, [this]() {
    while (m_httpServer.hasPendingConnections()) {

      QTcpSocket *socket = m_httpServer.nextPendingConnection();

      if (!socket)
        continue;

      /*
       * Per-connection state.
       *
       * We store the upload file and HTTP parsing state
       * as QObject properties on the socket.
       */

      socket->setProperty("headerComplete", false);

      socket->setProperty("contentLength", QVariant::fromValue<qlonglong>(-1));

      socket->setProperty("bytesReceived", QVariant::fromValue<qlonglong>(0));

      socket->setProperty("requestValid", false);

      socket->setProperty("processing", false);

      /*
       * Unique temporary filename per socket.
       */
      const QString inputPath =
          QDir(QDir::tempPath())
              .filePath(
                  QStringLiteral("qf-stt-%1-%2.mp4")
                      .arg(QString::number(QCoreApplication::applicationPid()))
                      .arg(
                          QString::number(reinterpret_cast<quintptr>(socket))));

      socket->setProperty("inputPath", inputPath);

      /*
       * Create the temporary MP4 immediately.
       */
      auto *file = new QFile(inputPath, socket);

      if (!file->open(QIODevice::WriteOnly)) {

        qWarning() << "[NemoTranscriber] Cannot create"
                   << "temporary upload:" << inputPath;

        sendError(socket, 500, "Internal Server Error",
                  QStringLiteral("Failed to create temporary upload file."));

        continue;
      }

      /*
       * Store the QFile pointer on the socket.
       */
      socket->setProperty("uploadFile", QVariant::fromValue<void *>(file));

      /*
       * Handle incoming data.
       */
      connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (!socket)
          return;

        if (!socket->isOpen())
          return;

        /*
         * Once processing has started, don't accept
         * additional request data.
         */
        if (socket->property("processing").toBool()) {
          return;
        }

        QByteArray buffer = socket->property("headerBuffer").toByteArray();

        /*
         * If the HTTP headers have not yet been
         * completely received, read enough data to
         * locate \r\n\r\n.
         */
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

          /*
           * Separate headers and the beginning
           * of the body.
           */
          const QByteArray headers = buffer.left(headerEnd);

          const QByteArray initialBody = buffer.mid(headerEnd + 4);

          socket->setProperty("headerComplete", true);

          /*
           * Validate request line.
           */
          const QList<QByteArray> lines = headers.split('\n');

          if (lines.isEmpty()) {

            sendError(socket, 400, "Bad Request",
                      QStringLiteral("Invalid HTTP request."));

            return;
          }

          const QByteArray requestLine = lines.first().trimmed();

          if (!requestLine.startsWith("POST /transcribe ")) {

            sendError(socket, 404, "Not Found",
                      QStringLiteral("Endpoint not found."));

            return;
          }

          /*
           * Parse headers.
           */
          qint64 contentLength = -1;

          for (int i = 1; i < lines.size(); ++i) {

            const QByteArray line = lines.at(i).trimmed();

            const int colon = line.indexOf(':');

            if (colon <= 0)
              continue;

            const QByteArray name = line.left(colon).trimmed().toLower();

            const QByteArray value = line.mid(colon + 1).trimmed();

            if (name == "content-length") {

              bool ok = false;

              const qlonglong length = value.toLongLong(&ok);

              if (!ok || length < 0) {

                sendError(socket, 400, "Bad Request",
                          QStringLiteral("Invalid Content-Length."));

                return;
              }

              contentLength = length;
            }
          }

          /*
           * Content-Length is required because
           * we're intentionally not implementing
           * chunked transfer encoding in this
           * temporary server.
           */
          if (contentLength < 0) {

            sendError(socket, 411, "Length Required",
                      QStringLiteral("Content-Length is required."));

            return;
          }

          if (contentLength == 0) {

            sendError(socket, 400, "Bad Request",
                      QStringLiteral("Empty upload."));

            return;
          }

          if (contentLength > MAX_UPLOAD_SIZE) {

            sendError(socket, 413, "Payload Too Large",
                      QStringLiteral("Upload exceeds the temporary "
                                     "2 GB limit."));

            return;
          }

          socket->setProperty("contentLength",
                              QVariant::fromValue<qlonglong>(contentLength));

          /*
           * Get the upload QFile.
           */
          QFile *file = static_cast<QFile *>(
              socket->property("uploadFile").value<void *>());

          if (!file) {

            sendError(socket, 500, "Internal Server Error",
                      QStringLiteral("Upload file is unavailable."));

            return;
          }

          /*
           * Write the body bytes that arrived
           * together with the HTTP headers.
           */
          if (!initialBody.isEmpty()) {

            const qint64 written = file->write(initialBody);

            if (written != initialBody.size()) {

              sendError(socket, 500, "Internal Server Error",
                        QStringLiteral("Failed to write upload."));

              return;
            }

            socket->setProperty("bytesReceived",
                                QVariant::fromValue<qlonglong>(written));
          }

          /*
           * Don't keep header bytes around.
           */
          socket->setProperty("headerBuffer", QByteArray());

        } else {

          /*
           * Headers are already complete.
           * Everything arriving now is body data.
           */
          const QByteArray data = socket->readAll();

          if (data.isEmpty())
            return;

          QFile *file = static_cast<QFile *>(
              socket->property("uploadFile").value<void *>());

          if (!file) {

            sendError(socket, 500, "Internal Server Error",
                      QStringLiteral("Upload file is unavailable."));

            return;
          }

          const qint64 previous =
              socket->property("bytesReceived").toLongLong();

          const qint64 contentLength =
              socket->property("contentLength").toLongLong();

          const qint64 remaining = contentLength - previous;

          /*
           * Never write beyond Content-Length.
           */
          const qint64 toWrite = std::min(remaining, data.size());

          if (toWrite > 0) {

            const qint64 written = file->write(data.constData(), toWrite);

            if (written != toWrite) {

              sendError(socket, 500, "Internal Server Error",
                        QStringLiteral("Failed to write upload."));

              return;
            }

            socket->setProperty("bytesReceived", QVariant::fromValue<qlonglong>(
                                                     previous + written));
          }
        }

        /*
         * Check whether the entire MP4 has arrived.
         */
        const qint64 received = socket->property("bytesReceived").toLongLong();

        const qint64 contentLength =
            socket->property("contentLength").toLongLong();

        if (contentLength <= 0 || received < contentLength) {
          return;
        }

        /*
         * Upload complete.
         */
        socket->setProperty("processing", true);

        QFile *file = static_cast<QFile *>(
            socket->property("uploadFile").value<void *>());

        if (file) {
          file->flush();
          file->close();
        }

        const QString inputPath = socket->property("inputPath").toString();

        const QString pcmPath = inputPath + QStringLiteral(".pcm");

        qDebug() << "[NemoTranscriber] Upload complete:" << received << "bytes";

        qDebug() << "[NemoTranscriber] Running FFmpeg:" << inputPath;

        /*
         * Convert:
         *
         * MP4
         *  -> audio only
         *  -> mono
         *  -> 16 kHz
         *  -> float32 little-endian PCM
         */
        QProcess ffmpeg;

        QStringList args;

        args << "-hide_banner"
             << "-loglevel" << "error"
             << "-y"
             << "-i" << inputPath << "-vn"
             << "-ar" << "16000"
             << "-ac" << "1"
             << "-f" << "f32le" << pcmPath;

        ffmpeg.start(QStringLiteral("ffmpeg"), args);

        if (!ffmpeg.waitForStarted(5000)) {

          QFile::remove(inputPath);

          if (file)
            file->deleteLater();

          sendError(socket, 500, "Internal Server Error",
                    QStringLiteral("Failed to start FFmpeg."));

          return;
        }

        const bool finished = ffmpeg.waitForFinished(-1);

        if (!finished || ffmpeg.exitStatus() != QProcess::NormalExit ||
            ffmpeg.exitCode() != 0) {

          const QString ffmpegError =
              QString::fromLocal8Bit(ffmpeg.readAllStandardError()).trimmed();

          QFile::remove(inputPath);
          QFile::remove(pcmPath);

          if (file)
            file->deleteLater();

          sendError(socket, 500, "Internal Server Error",
                    ffmpegError.isEmpty() ? QStringLiteral("FFmpeg failed.")
                                          : ffmpegError);

          return;
        }

        /*
         * MP4 is no longer needed.
         */
        QFile::remove(inputPath);

        if (file)
          file->deleteLater();

        /*
         * Read generated float32 PCM.
         */
        QFile pcmFile(pcmPath);

        if (!pcmFile.open(QIODevice::ReadOnly)) {

          QFile::remove(pcmPath);

          sendError(socket, 500, "Internal Server Error",
                    QStringLiteral("Failed to open generated PCM."));

          return;
        }

        const qint64 pcmSize = pcmFile.size();

        if (pcmSize <= 0 || pcmSize % static_cast<qint64>(sizeof(float)) != 0) {

          pcmFile.close();
          QFile::remove(pcmPath);

          sendError(socket, 500, "Internal Server Error",
                    QStringLiteral("Invalid generated PCM."));

          return;
        }

        /*
         * Read PCM into vector<float>.
         *
         * NeMo-Speech expects mono float32 PCM.
         */
        std::vector<float> pcm32f;

        pcm32f.resize(static_cast<size_t>(pcmSize / sizeof(float)));

        const qint64 expectedBytes = pcmSize;

        const qint64 readBytes = pcmFile.read(
            reinterpret_cast<char *>(pcm32f.data()), expectedBytes);

        pcmFile.close();
        QFile::remove(pcmPath);

        if (readBytes != expectedBytes) {

          sendError(socket, 500, "Internal Server Error",
                    QStringLiteral("Failed to read complete PCM."));

          return;
        }

        qDebug() << "[NemoTranscriber] PCM:" << pcm32f.size() << "samples"
                 << "(" << (pcm32f.size() / 16000.0) << "seconds )";

        /*
         * Existing Nemotron transcription path.
         */
        const QString text = transcribe(pcm32f);

        /*
         * Return JSON.
         */
        const QJsonObject json{{QStringLiteral("text"), text}};

        socket->write(jsonResponse(200, "OK", json));

        socket->disconnectFromHost();
      });

      /*
       * Clean up the socket and temporary file if the
       * client disconnects unexpectedly.
       */
      connect(socket, &QTcpSocket::disconnected, this, [socket]() {
        const QString inputPath = socket->property("inputPath").toString();

        if (!inputPath.isEmpty())
          QFile::remove(inputPath);

        const QString pcmPath = inputPath + QStringLiteral(".pcm");

        if (!pcmPath.isEmpty())
          QFile::remove(pcmPath);

        QFile *file = static_cast<QFile *>(
            socket->property("uploadFile").value<void *>());

        if (file) {
          if (file->isOpen())
            file->close();

          file->deleteLater();
        }

        socket->deleteLater();
      });
    }
  });

  /*
   * Localhost only.
   *
   * Nothing is exposed to your LAN.
   */
  if (!m_httpServer.listen(QHostAddress::LocalHost, HTTP_PORT)) {

    qWarning() << "[NemoTranscriber] Failed to start HTTP server:"
               << m_httpServer.errorString();

  } else {

    qDebug() << "[NemoTranscriber] Temporary STT HTTP endpoint:"
             << "http://127.0.0.1:" << HTTP_PORT << "/transcribe";
  }
}

NemoTranscriber::~NemoTranscriber() {
  m_httpServer.close();

  if (m_recognizer) {

    nemo_speech_asr_destroy(m_recognizer);

    m_recognizer = nullptr;
  }
}

bool NemoTranscriber::isLoaded() const { return m_recognizer != nullptr; }

QString NemoTranscriber::transcribe(const std::vector<float> &pcm32f) {
  if (!m_recognizer) {

    const QString errorText =
        QStringLiteral("NeMo-Speech recognizer is not loaded.");

    qWarning() << "[NemoTranscriber]" << errorText;

    emit transcriptionError(errorText);

    return {};
  }

  if (pcm32f.empty())
    return {};

  nemo_speech_asr_recognition_options options =
      nemo_speech_asr_recognition_options_default();

  options.enable_automatic_punctuation = true;

  /*
   * NeMo-Speech.cpp accepts mono float32 PCM.
   *
   * The audio we generate through FFmpeg is:
   *
   *   16000 Hz
   *   mono
   *   float32
   */
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

  if (text.isEmpty())
    return {};

  emit transcriptionFinished(text);

  return text;
}
