#include "../../include/inference/LlmClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

LlmClient::LlmClient(QNetworkAccessManager *networkManager, QObject *parent)
    : QObject(parent), m_networkManager(networkManager) {}

void LlmClient::sendRequest(const Request &request) {
  // If we are currently inside onFinished, do not touch m_currentReply
  // here: the outer frame still owns it and will clean up when it
  // returns. Queue the request and let onFinished deliver it.
  if (m_inFinished) {
    m_queuedRequest = request;
    m_hasQueuedRequest = true;

    qDebug() << "[LlmClient] Queued request (reentrant) url=" << request.url
             << "model=" << request.model;

    return;
  }

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

  QNetworkReply *reply = m_networkManager->post(
      networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));

  m_currentReply = reply;

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
  // If we are inside onFinished for the current reply, we must not
  // touch it. Defer the abort; the outer onFinished frame will see
  // m_deferredAbort and clean up.
  if (m_inFinished) {
    m_deferredAbort = true;
    m_hasQueuedRequest = false;   // queued request is now moot

    qDebug() << "[LlmClient] abortRequest deferred (in finished)";

    return;
  }

  QPointer<QNetworkReply> reply = m_currentReply;

  if (!reply) {
    return;
  }

  m_currentReply = nullptr;

  m_abortRequested = true;

  // Disconnect first so any in-flight queued signals from this reply do
  // not reach us after we have moved on.
  reply->disconnect(this);

  reply->abort();
  reply->deleteLater();

  m_streamBuffer.clear();
  m_currentSseData.clear();
  m_toolCallAccumulators.clear();

  qDebug() << "[LlmClient] Aborted reply=" << reply.data();
}

bool LlmClient::isActive() const { return !m_currentReply.isNull(); }

void LlmClient::onReadyRead() {
  if (m_inFinished) {
    return;
  }

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

  // If the reply is no longer current (a new request was started and
  // this reply is stale), drop it immediately. Never touch anything
  // else from here.
  if (reply != m_currentReply) {
    qDebug() << "[LlmClient] Ignoring stale finished reply=" << reply;
    reply->disconnect(this);
    reply->deleteLater();
    return;
  }

  // Mark reentrancy guard *before* we touch state or emit any signal.
  // Any sendRequest/abortRequest that runs during a signal emission
  // will queue instead of racing us.
  m_inFinished = true;

  // Detach this reply from m_currentReply immediately. From this point
  // on, m_currentReply is null and any reentrant sendRequest will start
  // cleanly, but we still own `reply` locally.
  m_currentReply = nullptr;

  // Disconnect everything from this reply. Anything queued from it
  // after this point will not reach us.
  reply->disconnect(this);

  // Snapshot the state we need before emitting.
  const bool aborted = m_abortRequested;
  const bool failed = m_requestFailed;
  const QNetworkReply::NetworkError error = reply->error();

  const int statusCode =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  qDebug() << "[LlmClient] Finished reply=" << reply << "aborted=" << aborted
           << "failed=" << failed << "error=" << error
           << "httpStatus=" << statusCode;

  // -------------------------------------------------------------------
  // Decide what signals we will emit. We capture them into locals and
  // reset our own state *before* emitting, so a reentrant sendRequest
  // starts from a clean slate.
  // -------------------------------------------------------------------

  enum class Outcome { None, Finished, ToolCalls, Error };

  Outcome outcome = Outcome::None;

  QJsonArray toolCalls;
  QString errorMessage;

  if (aborted) {
    outcome = Outcome::None;
  } else if (statusCode > 0 && (statusCode < 200 || statusCode >= 300)) {
    outcome = Outcome::Error;
    errorMessage = buildReplyError(reply);
  } else if (!failed && error == QNetworkReply::NoError) {
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

    toolCalls = finaliseToolCalls();

    if (!toolCalls.isEmpty()) {
      outcome = Outcome::ToolCalls;
    } else {
      outcome = Outcome::Finished;
    }
  } else {
    outcome = Outcome::Error;
    errorMessage = buildReplyError(reply);
  }

  // Reset all per-request state now, before any signal emission.
  m_streamBuffer.clear();
  m_currentSseData.clear();
  m_toolCallAccumulators.clear();
  m_abortRequested = false;
  m_requestFailed = false;

  // The reply itself is done. Schedule its deletion for after we return
  // to the event loop, so any downstream code that still holds a raw
  // pointer to it (unlikely but possible) sees a live object during the
  // signals below.
  reply->deleteLater();

  // Emit. Any of these emissions can cause a reentrant sendRequest or
  // abortRequest; those will queue because m_inFinished is still true.
  switch (outcome) {
  case Outcome::None:
    break;
  case Outcome::Finished:
    emit requestFinished();
    break;
  case Outcome::ToolCalls:
    emit toolCallsReceived(toolCalls);
    break;
  case Outcome::Error:
    emit requestError(errorMessage);
    break;
  }

  // Release the guard.
  m_inFinished = false;

  // If something requested an abort during emission, honour it now.
  if (m_deferredAbort) {
    m_deferredAbort = false;

    qDebug() << "[LlmClient] Delivering deferred abort";
  }

  // If something queued a new request during emission, deliver it now.
  if (m_hasQueuedRequest) {
    Request pending = m_queuedRequest;
    m_hasQueuedRequest = false;
    m_queuedRequest = Request();

    qDebug() << "[LlmClient] Delivering queued request url=" << pending.url;

    // Send it now. This runs from a fresh state and will not be
    // reentrant with the frame above.
    sendRequest(pending);
  }
}

void LlmClient::onError(QNetworkReply::NetworkError error) {
  Q_UNUSED(error)

  if (m_inFinished) {
    return;
  }

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