#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>

class LlmClient : public QObject {
  Q_OBJECT

public:
  enum class AuthType { None, Bearer };

  struct Request {
    QString url;

    AuthType authType = AuthType::None;

    QString apiKey;

    QJsonArray messages;

    QString model = QStringLiteral("local-model");

    double temperature = 0.7;

    int timeoutMs = 120000;

    /*
     * llama.cpp GBNF grammar.
     *
     * Used only by local inference.
     */
    QString grammar;

    /*
     * OpenAI/OpenRouter-compatible structured output.
     *
     * This should contain the complete response_format object,
     * e.g.:
     *
     * {
     *   "type": "json_schema",
     *   "json_schema": {
     *     "name": "edit_plan",
     *     "strict": true,
     *     "schema": { ... }
     *   }
     * }
     *
     * Used only by remote inference.
     */
    QJsonObject responseFormat;

    /*
     * OpenAI-compatible tool definitions. Each entry has the shape:
     *
     * {
     *   "type": "function",
     *   "function": {
     *     "name": "write_file",
     *     "description": "...",
     *     "parameters": { ... JSON schema ... }
     *   }
     * }
     *
     * Empty means no tools are exposed for this request.
     */
    QJsonArray tools;
  };

  explicit LlmClient(QNetworkAccessManager *networkManager,
                     QObject *parent = nullptr);

  void sendRequest(const Request &request);

  void abortRequest();

  bool isActive() const;

signals:
  void deltaReceived(const QString &text);

  void requestFinished();

  // Emitted instead of requestFinished when the response contained
  // tool_calls. The array is in OpenAI response format: each entry has
  // "id", "type", and "function": {"name": ..., "arguments": ...}.
  void toolCallsReceived(const QJsonArray &toolCalls);

  void requestError(const QString &error);

private slots:
  void onReadyRead();

  void onFinished();

  void onError(QNetworkReply::NetworkError error);

private:
  void consumeStreamBuffer();

  void processSseLine(const QByteArray &line);

  void dispatchSseMessage(const QByteArray &rawPayload);

  void accumulateToolCallDelta(const QJsonArray &deltas);

  QJsonArray finaliseToolCalls() const;

  QString buildReplyError(QNetworkReply *reply) const;

  // Drains any request that was queued during a signal emission.
  void deliverQueuedRequest();

private:
  QNetworkAccessManager *m_networkManager = nullptr;

  QPointer<QNetworkReply> m_currentReply;

  QByteArray m_streamBuffer;

  QByteArray m_currentSseData;

  struct ToolCallAccumulator {
    QString id;
    QString name;
    QString arguments;
  };
  QMap<int, ToolCallAccumulator> m_toolCallAccumulators;

  bool m_requestFailed = false;

  bool m_abortRequested = false;

  // True while we are executing onFinished. Used to make sendRequest and
  // abortRequest reentrancy-safe: a request started from inside a
  // finished/toolCalls/error slot must be queued, not applied
  // immediately, because the outer onFinished frame still owns the
  // current reply.
  bool m_inFinished = false;

  // A request queued while m_inFinished was true. Applied when the outer
  // onFinished frame returns.
  bool m_hasQueuedRequest = false;
  Request m_queuedRequest;

  // Set when abortRequest() is called while m_inFinished is true. The
  // outer onFinished frame honours it before returning.
  bool m_deferredAbort = false;
};