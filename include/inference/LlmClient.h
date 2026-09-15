#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
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

private:
  QNetworkAccessManager *m_networkManager = nullptr;

  QNetworkReply *m_currentReply = nullptr;

  QByteArray m_streamBuffer;

  QByteArray m_currentSseData;

  // Tool-call fragments accumulated across SSE deltas, keyed by the
  // "index" field the API assigns. Each entry holds id, name, and the
  // accumulated arguments string.
  struct ToolCallAccumulator {
    QString id;
    QString name;
    QString arguments;
  };
  QMap<int, ToolCallAccumulator> m_toolCallAccumulators;

  bool m_requestFailed = false;

  bool m_abortRequested = false;
};