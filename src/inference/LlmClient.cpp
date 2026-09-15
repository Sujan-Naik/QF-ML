#include "../../include/inference/LlmClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

LlmClient::LlmClient(QNetworkAccessManager *networkManager, QObject *parent)
    : QObject(parent), m_networkManager(networkManager) {}

void LlmClient::sendRequest(const Request &request) {
  abortRequest();

  if (!m_networkManager) {
    emit requestError(QStringLiteral("LLM network manager is unavailable."));
    return;
  }

  QUrl url(request.url);

  if (url.scheme().isEmpty()) {
    url.setScheme(QStringLiteral("http"));
  }

  if (!url.isValid() || url.isEmpty()) {
    emit requestError(
        QStringLiteral("Invalid LLM URL: %1").arg(url.errorString()));
    return;
  }

  if (url.host().isEmpty()) {
    emit requestError(
        QStringLiteral("LLM URL has no host: %1").arg(request.url));
    return;
  }

  QNetworkRequest networkRequest(url);

  networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                           QStringLiteral("application/json"));

  networkRequest.setRawHeader(QByteArrayLiteral("Accept"),
                              QByteArrayLiteral("text/event-stream"));

  networkRequest.setHeader(QNetworkRequest::UserAgentHeader,
                           QStringLiteral("TalosApp/1.0"));

  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);

  networkRequest.setTransferTimeout(request.timeoutMs);

  if (request.authType == AuthType::Bearer && !request.apiKey.isEmpty()) {
    networkRequest.setRawHeader(QByteArrayLiteral("Authorization"),
                                QByteArrayLiteral("Bearer ") +
                                    request.apiKey.toUtf8());
  }

  QJsonObject body;

  body.insert(QStringLiteral("model"), request.model);

  body.insert(QStringLiteral("messages"), request.messages);

  body.insert(QStringLiteral("stream"), true);

  body.insert(QStringLiteral("temperature"), request.temperature);

  if (!request.grammar.isEmpty()) {
    body.insert(QStringLiteral("grammar"), request.grammar);
  }

  if (!request.responseFormat.isEmpty()) {
    body.insert(QStringLiteral("response_format"), request.responseFormat);
  }

  if (!request.tools.isEmpty()) {
    body.insert(QStringLiteral("tools"), request.tools);
    body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
  }

  m_streamBuffer.clear();
  m_currentSseData.clear();
  m_toolCallAccumulators.clear();

  m_requestFailed = false;

  m_abortRequested = false;

  m_currentReply = m_networkManager->post(
      networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));

  QNetworkReply *reply = m_currentReply;

  connect(reply, &QNetworkReply::readyRead, this, &LlmClient::onReadyRead);

  connect(reply, &QNetworkReply::finished, this, &LlmClient::onFinished);

  connect(
      reply,
      QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::errorOccurred),
      this, &LlmClient::onError);

  qDebug() << "[LlmClient] POST started"
           << "reply=" << reply << "url=" << url << "model=" << request.model
           << "authenticated="
           << (request.authType == AuthType::Bearer &&
               !request.apiKey.isEmpty())
           << "hasGrammar=" << !request.grammar.isEmpty()
           << "hasResponseFormat=" << !request.responseFormat.isEmpty()
           << "toolCount=" << request.tools.size();
}

void LlmClient::abortRequest() {
  QNetworkReply *reply = m_currentReply;

  if (!reply) {
    return;
  }

  m_currentReply = nullptr;

  m_abortRequested = true;

  reply->abort();

  reply->deleteLater();

  m_streamBuffer.clear();
  m_currentSseData.clear();
  m_toolCallAccumulators.clear();

  qDebug() << "[LlmClient] Aborted reply=" << reply;
}

bool LlmClient::isActive() const { return m_currentReply != nullptr; }

void LlmClient::onReadyRead() {
  auto *reply = qobject_cast<QNetworkReply *>(sender());

  if (!reply || reply != m_currentReply || m_requestFailed ||
      m_abortRequested) {
    return;
  }

  const QByteArray data = reply->readAll();

  if (data.isEmpty()) {
    return;
  }

  m_streamBuffer.append(data);

  consumeStreamBuffer();
}

void LlmClient::consumeStreamBuffer() {
  while (true) {
    const int lineEndIndex = m_streamBuffer.indexOf('\n');

    if (lineEndIndex == -1) {
      break;
    }

    int lineLength = lineEndIndex;

    if (lineLength > 0 && m_streamBuffer.at(lineLength - 1) == '\r') {
      lineLength--;
    }

    const QByteArray line = m_streamBuffer.left(lineLength);

    m_streamBuffer.remove(0, lineEndIndex + 1);

    processSseLine(line);
  }
}

void LlmClient::processSseLine(const QByteArray &line) {
  if (line.isEmpty()) {
    if (!m_currentSseData.isEmpty()) {
      dispatchSseMessage(m_currentSseData);
      m_currentSseData.clear();
    }
    return;
  }

  if (line.startsWith(':')) {
    return;
  }

  const int colonIndex = line.indexOf(':');

  QByteArray fieldName;
  QByteArray fieldValue;

  if (colonIndex != -1) {
    fieldName = line.left(colonIndex);
    fieldValue = line.mid(colonIndex + 1);
    if (fieldValue.startsWith(' ')) {
      fieldValue.remove(0, 1);
    }
  } else {
    fieldName = line;
  }

  if (fieldName == "data") {
    if (!m_currentSseData.isEmpty()) {
      m_currentSseData.append('\n');
    }
    m_currentSseData.append(fieldValue);
  }
}

void LlmClient::accumulateToolCallDelta(const QJsonArray &deltas) {
  for (const QJsonValue &value : deltas) {
    if (!value.isObject()) {
      continue;
    }

    const QJsonObject delta = value.toObject();

    const int index = delta.value(QStringLiteral("index")).toInt(0);

    ToolCallAccumulator &accumulator = m_toolCallAccumulators[index];

    const QString id = delta.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
      accumulator.id = id;
    }

    const QJsonObject function =
        delta.value(QStringLiteral("function")).toObject();

    if (!function.isEmpty()) {
      const QString name =
          function.value(QStringLiteral("name")).toString();
      if (!name.isEmpty()) {
        accumulator.name = name;
      }

      const QString arguments =
          function.value(QStringLiteral("arguments")).toString();
      if (!arguments.isEmpty()) {
        accumulator.arguments += arguments;
      }
    }
  }
}

QJsonArray LlmClient::finaliseToolCalls() const {
  QJsonArray result;

  // QMap iteration is sorted by key, which preserves the model's ordering.
  for (auto it = m_toolCallAccumulators.constBegin();
       it != m_toolCallAccumulators.constEnd(); ++it) {
    const ToolCallAccumulator &accumulator = it.value();

    if (accumulator.name.isEmpty()) {
      continue;
    }

    QJsonObject function;
    function.insert(QStringLiteral("name"), accumulator.name);
    function.insert(QStringLiteral("arguments"), accumulator.arguments);

    QJsonObject call;
    call.insert(QStringLiteral("id"),
                accumulator.id.isEmpty()
                    ? QStringLiteral("call_%1").arg(it.key())
                    : accumulator.id);
    call.insert(QStringLiteral("type"), QStringLiteral("function"));
    call.insert(QStringLiteral("function"), function);

    result.append(call);
  }

  return result;
}

void LlmClient::dispatchSseMessage(const QByteArray &rawPayload) {
  const QByteArray payload = rawPayload.trimmed();

  if (payload.isEmpty()) {
    return;
  }

  if (payload == "[DONE]") {
    return;
  }

  QJsonParseError parseError;

  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);

  if (parseError.error != QJsonParseError::NoError) {
    qWarning() << "[LLM] Failed to parse SSE JSON payload:"
               << parseError.errorString() << payload;
    return;
  }

  if (!document.isObject()) {
    return;
  }

  const QJsonObject root = document.object();

  const QJsonValue choicesValue = root.value(QStringLiteral("choices"));

  if (!choicesValue.isArray()) {
    return;
  }

  const QJsonArray choices = choicesValue.toArray();

  if (choices.isEmpty()) {
    return;
  }

  const QJsonObject choice = choices.first().toObject();

  QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

  if (delta.isEmpty()) {
    delta = choice.value(QStringLiteral("message")).toObject();
  }

  const QString content = delta.value(QStringLiteral("content")).toString();

  if (!content.isEmpty()) {
    emit deltaReceived(content);
  }

  const QJsonValue toolCallsValue =
      delta.value(QStringLiteral("tool_calls"));

  if (toolCallsValue.isArray()) {
    accumulateToolCallDelta(toolCallsValue.toArray());
  }
}

QString LlmClient::buildReplyError(QNetworkReply *reply) const {
  if (!reply) {
    return QStringLiteral("LLM request failed.");
  }

  const int statusCode =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  const QByteArray body = reply->readAll();

  QString message;

  QJsonParseError parseError;

  const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);

  if (parseError.error == QJsonParseError::NoError && document.isObject()) {
    const QJsonObject root = document.object();

    const QJsonObject errorObject =
        root.value(QStringLiteral("error")).toObject();

    if (!errorObject.isEmpty()) {
      message = errorObject.value(QStringLiteral("message")).toString();
      if (message.isEmpty()) {
        message = errorObject.value(QStringLiteral("detail")).toString();
      }
    }

    if (message.isEmpty()) {
      message = root.value(QStringLiteral("message")).toString();
    }
  }

  if (message.isEmpty()) {
    message = QString::fromUtf8(body).trimmed();
  }

  QString result = QStringLiteral("LLM request failed");

  if (statusCode > 0) {
    result += QStringLiteral(" (HTTP %1)").arg(statusCode);
  }

  const QString networkError = reply->errorString().trimmed();

  if (!message.isEmpty()) {
    result += QStringLiteral(": ") + message;
  } else if (!networkError.isEmpty()) {
    result += QStringLiteral(": ") + networkError;
  }

  return result;
}

void LlmClient::onFinished() {
  auto *reply = qobject_cast<QNetworkReply *>(sender());

  if (!reply) {
    return;
  }

  if (reply != m_currentReply) {
    qDebug() << "[LlmClient] Ignoring stale finished reply=" << reply;
    reply->deleteLater();
    return;
  }

  m_currentReply = nullptr;

  const bool aborted = m_abortRequested;

  const bool failed = m_requestFailed;

  const QNetworkReply::NetworkError error = reply->error();

  const int statusCode =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  qDebug() << "[LlmClient] Finished reply=" << reply << "aborted=" << aborted
           << "failed=" << failed << "error=" << error
           << "httpStatus=" << statusCode;

  if (aborted) {
    reply->deleteLater();

    m_streamBuffer.clear();
    m_currentSseData.clear();
    m_toolCallAccumulators.clear();

    m_abortRequested = false;
    m_requestFailed = false;

    return;
  }

  if (statusCode > 0 && (statusCode < 200 || statusCode >= 300)) {
    m_requestFailed = true;

    emit requestError(buildReplyError(reply));

    reply->deleteLater();

    m_streamBuffer.clear();
    m_currentSseData.clear();
    m_toolCallAccumulators.clear();

    m_abortRequested = false;
    m_requestFailed = false;

    return;
  }

  if (!failed && error == QNetworkReply::NoError) {
    consumeStreamBuffer();

    if (!m_streamBuffer.isEmpty()) {
      QByteArray remaining = m_streamBuffer;
      m_streamBuffer.clear();
      if (remaining.endsWith('\r')) {
        remaining.chop(1);
      }
      processSseLine(remaining);
    }

    if (!m_currentSseData.isEmpty()) {
      dispatchSseMessage(m_currentSseData);
      m_currentSseData.clear();
    }

    const QJsonArray toolCalls = finaliseToolCalls();

    if (!toolCalls.isEmpty()) {
      emit toolCallsReceived(toolCalls);
    } else {
      emit requestFinished();
    }

  } else if (!m_requestFailed) {
    m_requestFailed = true;
    emit requestError(buildReplyError(reply));
  }

  reply->deleteLater();

  m_streamBuffer.clear();
  m_currentSseData.clear();
  m_toolCallAccumulators.clear();

  m_abortRequested = false;
  m_requestFailed = false;
}

void LlmClient::onError(QNetworkReply::NetworkError error) {
  Q_UNUSED(error)

  auto *reply = qobject_cast<QNetworkReply *>(sender());

  if (!reply) {
    return;
  }

  if (reply != m_currentReply) {
    qDebug() << "[LlmClient] Ignoring stale error reply=" << reply;
    return;
  }

  if (m_abortRequested) {
    return;
  }

  m_requestFailed = true;
}