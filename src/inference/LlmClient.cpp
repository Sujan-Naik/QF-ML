#include "../../include/inference/LlmClient.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

// True when `segment`, taken as an SSE data payload, looks like the
// start of a new SSE message rather than a continuation line of the
// current one. A new message is either the literal [DONE] sentinel or
// a single JSON value (object or array). A continuation line of a
// multi-line JSON payload does not start with '{' or '[' — it starts
// with a key, a string, or a scalar — so this check splits
// "{...}\n[DONE]" and "{...}\n{...}" correctly while leaving a genuine
// multi-line JSON payload intact.
bool looksLikeNewSsePayload(const QByteArray &segment) {
  const QByteArray trimmed = segment.trimmed();

  if (trimmed.isEmpty())
    return false;

  if (trimmed == QByteArrayLiteral("[DONE]"))
    return true;

  const char first = trimmed.at(0);

  return first == '{' || first == '[';
}

// Split `buffer` into one or more SSE payloads at points where a
// new payload begins. Returns the segments in order. An empty buffer
// returns an empty list.
QList<QByteArray> splitSsePayloads(const QByteArray &buffer) {
  QList<QByteArray> result;

  if (buffer.isEmpty())
    return result;

  const QList<QByteArray> lines = buffer.split('\n');

  QByteArray current;

  for (const QByteArray &line : lines) {
    if (looksLikeNewSsePayload(line) && !current.isEmpty()) {
      result.append(current);
      current.clear();
    }

    if (!current.isEmpty())
      current.append('\n');

    current.append(line);
  }

  if (!current.isEmpty())
    result.append(current);

  return result;
}

} // namespace

LlmClient::LlmClient(QNetworkAccessManager *networkManager, QObject *parent)
    : QObject(parent), m_networkManager(networkManager) {}

bool LlmClient::isCompleteJson(const QByteArray &payload) {
  QJsonParseError parseError;

  const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);

  if (parseError.error != QJsonParseError::NoError)
    return false;

  return doc.isObject() || doc.isArray();
}

LlmClient::Token LlmClient::sendRequest(const Request &request) {
  const Token token = QUuid::createUuid();

  if (!m_networkManager) {
    emit requestError(token,
                      QStringLiteral("LLM network manager is unavailable."));
    return token;
  }

  QUrl url(request.url);

  if (url.scheme().isEmpty())
    url.setScheme(QStringLiteral("http"));

  if (!url.isValid() || url.isEmpty()) {
    emit requestError(
        token, QStringLiteral("Invalid LLM URL: %1").arg(url.errorString()));
    return token;
  }

  if (url.host().isEmpty()) {
    emit requestError(
        token, QStringLiteral("LLM URL has no host: %1").arg(request.url));
    return token;
  }

  QNetworkRequest networkRequest(url);

  networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                           QStringLiteral("application/json"));

  networkRequest.setRawHeader(QByteArrayLiteral("Accept"),
                              QByteArrayLiteral("text/event-stream"));

  networkRequest.setHeader(QNetworkRequest::UserAgentHeader,
                           QStringLiteral("LoreApp/1.0"));

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

  if (!request.grammar.isEmpty())
    body.insert(QStringLiteral("grammar"), request.grammar);

  if (!request.responseFormat.isEmpty())
    body.insert(QStringLiteral("response_format"), request.responseFormat);

  if (!request.tools.isEmpty()) {
    body.insert(QStringLiteral("tools"), request.tools);
    body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
  }

  if (!request.sessionId.isEmpty())
    body.insert(QStringLiteral("session_id"), request.sessionId);

  ReplyState state;

  QNetworkReply *reply = m_networkManager->post(
      networkRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));

  state.reply = reply;

  m_states.insert(token, state);
  m_replyToToken.insert(reply, token);

  connect(reply, &QNetworkReply::readyRead, this, &LlmClient::onReadyRead);
  connect(reply, &QNetworkReply::finished, this, &LlmClient::onFinished);
  connect(
      reply,
      QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::errorOccurred),
      this, &LlmClient::onError);

  qDebug() << "[LlmClient] POST started"
           << "token=" << token << "reply=" << reply << "url=" << url
           << "model=" << request.model
           << "sessionId=" << request.sessionId;

  return token;
}

void LlmClient::abortRequest(const Token &token) {
  auto it = m_states.find(token);

  if (it == m_states.end())
    return;

  ReplyState &state = it.value();

  state.abortRequested = true;

  QNetworkReply *reply = state.reply;

  if (reply) {
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    m_replyToToken.remove(reply);
  }

  m_states.erase(it);

  qDebug() << "[LlmClient] Aborted token=" << token;
}

void LlmClient::abortAllRequests() {
  const QList<Token> tokens = m_states.keys();

  for (const Token &token : tokens)
    abortRequest(token);
}

bool LlmClient::isActive(const Token &token) const {
  return m_states.contains(token);
}

LlmClient::ReplyState *LlmClient::stateFor(QNetworkReply *reply) {
  if (!reply)
    return nullptr;

  auto it = m_replyToToken.find(reply);

  if (it == m_replyToToken.end())
    return nullptr;

  auto sit = m_states.find(it.value());

  if (sit == m_states.end())
    return nullptr;

  return &sit.value();
}

const LlmClient::ReplyState *LlmClient::stateFor(QNetworkReply *reply) const {
  return const_cast<LlmClient *>(this)->stateFor(reply);
}

void LlmClient::onReadyRead() {
  auto *reply = qobject_cast<QNetworkReply *>(sender());

  if (!reply)
    return;

  ReplyState *state = stateFor(reply);

  if (!state || state->requestFailed || state->abortRequested)
    return;

  const QByteArray data = reply->readAll();

  if (data.isEmpty())
    return;

  state->streamBuffer.append(data);

  consumeStreamBuffer(*state);
}

void LlmClient::consumeStreamBuffer(ReplyState &state) {
  while (true) {
    const int lineEndIndex = state.streamBuffer.indexOf('\n');

    if (lineEndIndex == -1)
      break;

    int lineLength = lineEndIndex;

    if (lineLength > 0 && state.streamBuffer.at(lineLength - 1) == '\r')
      lineLength--;

    const QByteArray line = state.streamBuffer.left(lineLength);

    state.streamBuffer.remove(0, lineEndIndex + 1);

    processSseLine(state, line);
  }
}

void LlmClient::flushStreamBuffer(ReplyState &state, const Token &token) {
  // The SSE grammar terminates a message with a blank line, so a
  // well-formed stream always ends with one. If we are here with
  // anything left in the buffer, the stream ended without a final
  // blank line, and the last one or two SSE messages — a content
  // chunk and the [DONE] sentinel, typically — are sitting in
  // state.currentSseData joined by a newline.
  //
  // Consume any complete lines still in the raw buffer. Each one is
  // fed through processSseLine, which accumulates data: fields into
  // currentSseData exactly as if a blank line had arrived.
  while (true) {
    const int newlineIndex = state.streamBuffer.indexOf('\n');

    if (newlineIndex == -1)
      break;

    int lineLength = newlineIndex;

    if (lineLength > 0 && state.streamBuffer.at(lineLength - 1) == '\r')
      lineLength--;

    const QByteArray line = state.streamBuffer.left(lineLength);

    state.streamBuffer.remove(0, newlineIndex + 1);

    processSseLine(state, line);
  }

  // If a bare JSON payload remains in the raw buffer with no data:
  // prefix and no trailing newline, feed it through the normal parser
  // as a synthetic data: line so it lands in currentSseData with
  // everything else.
  if (!state.streamBuffer.isEmpty()) {
    QByteArray remaining = state.streamBuffer;

    state.streamBuffer.clear();

    if (remaining.endsWith('\r'))
      remaining.chop(1);

    if (remaining.startsWith("data:")) {
      processSseLine(state, remaining);
    } else if (!remaining.trimmed().isEmpty()) {
      // Bare payload, no SSE prefix. Dispatch directly.
      dispatchSseMessage(token, state, remaining);
    }
  }

  // Whatever is in currentSseData now may hold one SSE message
  // (multi-line data joined by \n, dispatched as a unit) or two or
  // more messages that were never separated by a blank line. Split
  // only at points where a new SSE payload begins; a continuation
  // line of a genuine multi-line JSON payload does not begin with
  // '{', '[' or the [DONE] sentinel, so it is left intact.
  if (!state.currentSseData.isEmpty()) {
    const QList<QByteArray> payloads = splitSsePayloads(state.currentSseData);

    state.currentSseData.clear();

    for (const QByteArray &payload : payloads) {
      if (!payload.trimmed().isEmpty())
        dispatchSseMessage(token, state, payload);
    }
  }
}

void LlmClient::processSseLine(ReplyState &state, const QByteArray &line) {
  if (line.isEmpty()) {
    if (!state.currentSseData.isEmpty()) {
      QNetworkReply *reply = state.reply;

      if (!reply)
        return;

      auto it = m_replyToToken.find(reply);

      if (it == m_replyToToken.end())
        return;

      dispatchSseMessage(it.value(), state, state.currentSseData);

      state.currentSseData.clear();
    }
    return;
  }

  if (line.startsWith(':'))
    return;

  const int colonIndex = line.indexOf(':');

  QByteArray fieldName;
  QByteArray fieldValue;

  if (colonIndex != -1) {
    fieldName = line.left(colonIndex);
    fieldValue = line.mid(colonIndex + 1);
    if (fieldValue.startsWith(' '))
      fieldValue.remove(0, 1);
  } else {
    fieldName = line;
  }

  if (fieldName == "data") {
    if (!state.currentSseData.isEmpty())
      state.currentSseData.append('\n');

    state.currentSseData.append(fieldValue);
  }
}

void LlmClient::accumulateToolCallDelta(ReplyState &state,
                                        const QJsonArray &deltas) {
  for (const QJsonValue &value : deltas) {
    if (!value.isObject())
      continue;

    const QJsonObject delta = value.toObject();

    const int index = delta.value(QStringLiteral("index")).toInt(0);

    ToolCallAccumulator &accumulator = state.toolCallAccumulators[index];

    const QString id = delta.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
      accumulator.id = id;

    const QJsonObject function =
        delta.value(QStringLiteral("function")).toObject();

    if (!function.isEmpty()) {
      const QString name =
          function.value(QStringLiteral("name")).toString();
      if (!name.isEmpty())
        accumulator.name = name;

      const QString arguments =
          function.value(QStringLiteral("arguments")).toString();
      if (!arguments.isEmpty())
        accumulator.arguments += arguments;
    }
  }
}

QJsonArray LlmClient::finaliseToolCalls(const ReplyState &state) const {
  QJsonArray result;

  for (auto it = state.toolCallAccumulators.constBegin();
       it != state.toolCallAccumulators.constEnd(); ++it) {
    const ToolCallAccumulator &accumulator = it.value();

    if (accumulator.name.isEmpty())
      continue;

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

void LlmClient::dispatchSseMessage(const Token &token, ReplyState &state,
                                   const QByteArray &rawPayload) {
  const QByteArray payload = rawPayload.trimmed();

  if (payload.isEmpty())
    return;

  if (payload == "[DONE]") {
    state.sawDoneSentinel = true;
    return;
  }

  QJsonParseError parseError;

  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);

  if (parseError.error != QJsonParseError::NoError) {
    qWarning() << "[LLM] Failed to parse SSE JSON payload:"
               << parseError.errorString() << payload;
    return;
  }

  if (!document.isObject())
    return;

  const QJsonObject root = document.object();

  // Some providers deliver errors as an "error" object inside the SSE
  // stream rather than as an HTTP status. Surface them as requestError.
  const QJsonObject errorObject =
      root.value(QStringLiteral("error")).toObject();

  if (!errorObject.isEmpty()) {
    const QString message =
        errorObject.value(QStringLiteral("message")).toString();

    state.requestFailed = true;

    const QString errorText =
        message.isEmpty()
            ? QStringLiteral("Provider reported an error mid-stream.")
            : message;

    // Deliver the error after this function returns so that no
    // consumer can re-enter LlmClient and invalidate `state` (which
    // is a reference into m_states) before consumeStreamBuffer is
    // done with it.
    QMetaObject::invokeMethod(
        this,
        [this, token, errorText]() { emit requestError(token, errorText); },
        Qt::QueuedConnection);

    return;
  }

  const QJsonValue choicesValue = root.value(QStringLiteral("choices"));

  if (!choicesValue.isArray())
    return;

  const QJsonArray choices = choicesValue.toArray();

  if (choices.isEmpty())
    return;

  const QJsonObject choice = choices.first().toObject();

  QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

  if (delta.isEmpty())
    delta = choice.value(QStringLiteral("message")).toObject();

  const QString content = delta.value(QStringLiteral("content")).toString();

  if (!content.isEmpty()) {
    state.receivedAnyDelta = true;

    // Queued so that deltaReceived consumers run after
    // consumeStreamBuffer has finished iterating and released its
    // reference into m_states.
    QMetaObject::invokeMethod(
        this,
        [this, token, content]() { emit deltaReceived(token, content); },
        Qt::QueuedConnection);
  }

  const QJsonValue toolCallsValue =
      delta.value(QStringLiteral("tool_calls"));

  if (toolCallsValue.isArray() && !toolCallsValue.toArray().isEmpty()) {
    state.receivedAnyDelta = true;
    accumulateToolCallDelta(state, toolCallsValue.toArray());
  }
}

QString LlmClient::buildReplyError(QNetworkReply *reply) const {
  if (!reply)
    return QStringLiteral("LLM request failed.");

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
      if (message.isEmpty())
        message = errorObject.value(QStringLiteral("detail")).toString();
    }

    if (message.isEmpty())
      message = root.value(QStringLiteral("message")).toString();
  }

  if (message.isEmpty())
    message = QString::fromUtf8(body).trimmed();

  QString result = QStringLiteral("LLM request failed");

  if (statusCode > 0)
    result += QStringLiteral(" (HTTP %1)").arg(statusCode);

  const QString networkError = reply->errorString().trimmed();

  if (!message.isEmpty())
    result += QStringLiteral(": ") + message;
  else if (!networkError.isEmpty())
    result += QStringLiteral(": ") + networkError;

  return result;
}

void LlmClient::onFinished() {
  auto *reply = qobject_cast<QNetworkReply *>(sender());

  if (!reply)
    return;

  auto tokenIt = m_replyToToken.find(reply);

  if (tokenIt == m_replyToToken.end()) {
    reply->deleteLater();
    return;
  }

  const Token token = tokenIt.value();

  auto stateIt = m_states.find(token);

  if (stateIt == m_states.end()) {
    m_replyToToken.remove(reply);
    reply->deleteLater();
    return;
  }

  // Drain any bytes the reply still holds before the state is copied
  // out and the reply is torn down. On most Qt builds readyRead has
  // already consumed everything, so this is usually a no-op, but if the
  // final TCP segment delivered the last content frame together with
  // the [DONE] sentinel, those bytes are still in the reply here and
  // would otherwise be discarded with the reply.
  const QByteArray tail = reply->readAll();

  ReplyState state = stateIt.value();

  if (!tail.isEmpty())
    state.streamBuffer.append(tail);

  const bool aborted = state.abortRequested;
  const bool failed = state.requestFailed;
  const QNetworkReply::NetworkError error = reply->error();

  const int statusCode =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  qDebug() << "[LlmClient] Finished token=" << token
           << "aborted=" << aborted << "failed=" << failed
           << "error=" << error << "httpStatus=" << statusCode
           << "receivedAnyDelta=" << state.receivedAnyDelta
           << "sawDoneSentinel=" << state.sawDoneSentinel;

  m_replyToToken.remove(reply);
  m_states.remove(token);

  reply->disconnect(this);
  reply->deleteLater();

  if (aborted)
    return;

  if (statusCode > 0 && (statusCode < 200 || statusCode >= 300)) {
    const QString errorText = buildReplyError(reply);

    // Queued so it lands after any delta that dispatchSseMessage
    // already posted to the event loop for this token.
    QMetaObject::invokeMethod(
        this,
        [this, token, errorText]() { emit requestError(token, errorText); },
        Qt::QueuedConnection);
    return;
  }

  if (!failed && error == QNetworkReply::NoError) {
    // Consume any complete lines still in the buffer, then flush
    // whatever partial message remains so the final content delta is
    // not lost when the stream did not end with a newline.
    consumeStreamBuffer(state);
    flushStreamBuffer(state, token);

    if (state.requestFailed) {
      // dispatchSseMessage already queued a requestError.
      return;
    }

    const QJsonArray toolCalls = finaliseToolCalls(state);

    if (!toolCalls.isEmpty()) {
      QMetaObject::invokeMethod(
          this,
          [this, token, toolCalls]() {
            emit toolCallsReceived(token, toolCalls);
          },
          Qt::QueuedConnection);
      return;
    }

    // The stream must have produced content. If it did not, the
    // provider closed the connection without sending anything.
    if (!state.receivedAnyDelta) {
      const QString errorText =
          QStringLiteral("The LLM provider closed the stream without "
                         "sending any content. This is usually a "
                         "transient server-side failure. Retry.");

      QMetaObject::invokeMethod(
          this,
          [this, token, errorText]() { emit requestError(token, errorText); },
          Qt::QueuedConnection);
      return;
    }

    // OpenRouter and other OpenAI-compatible providers terminate the
    // SSE stream with a "data: [DONE]" sentinel. If the sentinel never
    // arrived, the stream was truncated mid-generation. Treat that as
    // an error rather than a clean finish, because the accumulated
    // content is incomplete.
    if (!state.sawDoneSentinel) {
      const QString errorText =
          QStringLiteral("The LLM stream ended without a [DONE] sentinel. "
                         "This means the response was truncated. Retry.");

      QMetaObject::invokeMethod(
          this,
          [this, token, errorText]() { emit requestError(token, errorText); },
          Qt::QueuedConnection);
      return;
    }

    QMetaObject::invokeMethod(
        this, [this, token]() { emit requestFinished(token); },
        Qt::QueuedConnection);
    return;
  }

  const QString errorText = buildReplyError(reply);

  QMetaObject::invokeMethod(
      this,
      [this, token, errorText]() { emit requestError(token, errorText); },
      Qt::QueuedConnection);
}

void LlmClient::onError(QNetworkReply::NetworkError error) {
  Q_UNUSED(error);

  auto *reply = qobject_cast<QNetworkReply *>(sender());

  if (!reply)
    return;

  ReplyState *state = stateFor(reply);

  if (!state)
    return;

  if (state->abortRequested)
    return;

  state->requestFailed = true;
}