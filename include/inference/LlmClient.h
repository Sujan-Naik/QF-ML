#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUuid>

class LlmClient : public QObject {
  Q_OBJECT

public:
  using Token = QUuid;

  enum class AuthType { None, Bearer };

  struct Request {
    QString url;
    AuthType authType = AuthType::None;
    QString apiKey;
    QJsonArray messages;
    QString model = QStringLiteral("local-model");
    double temperature = 0.7;
    int timeoutMs = 120000;
    QString grammar;
    QJsonObject responseFormat;
    QJsonArray tools;

    // A stable identifier for the conversation this request belongs
    // to. OpenRouter uses this as a sticky routing key so that all
    // requests with the same session id land on the same provider
    // endpoint and share its prompt cache. Empty means no preference.
    QString sessionId;
  };

  explicit LlmClient(QNetworkAccessManager *networkManager,
                     QObject *parent = nullptr);

  Token sendRequest(const Request &request);

  void abortRequest(const Token &token);
  void abortAllRequests();

  bool isActive(const Token &token) const;
  bool hasActiveRequests() const { return !m_states.isEmpty(); }

signals:
  void deltaReceived(const Token &token, const QString &text);
  void requestFinished(const Token &token);
  void toolCallsReceived(const Token &token, const QJsonArray &toolCalls);
  void requestError(const Token &token, const QString &error);

private slots:
  void onReadyRead();
  void onFinished();
  void onError(QNetworkReply::NetworkError error);

private:
  struct ToolCallAccumulator {
    QString id;
    QString name;
    QString arguments;
  };

  struct ReplyState {
    QPointer<QNetworkReply> reply;
    QByteArray streamBuffer;
    QByteArray currentSseData;
    QMap<int, ToolCallAccumulator> toolCallAccumulators;
    bool abortRequested = false;
    bool requestFailed = false;
    bool receivedAnyDelta = false;
    bool sawDoneSentinel = false;
  };

  ReplyState *stateFor(QNetworkReply *reply);
  const ReplyState *stateFor(QNetworkReply *reply) const;

  void consumeStreamBuffer(ReplyState &state);
  void processSseLine(ReplyState &state, const QByteArray &line);
  void dispatchSseMessage(const Token &token, ReplyState &state,
                          const QByteArray &rawPayload);
  void accumulateToolCallDelta(ReplyState &state, const QJsonArray &deltas);
  QJsonArray finaliseToolCalls(const ReplyState &state) const;
  QString buildReplyError(QNetworkReply *reply) const;

  // Flush whatever is left in the stream buffer at end-of-stream. The
  // SSE grammar terminates a message with a blank line, so a well-
  // formed stream always ends with a newline. If the buffer does not,
  // the final message was truncated. Split on any complete data:
  // boundaries that are present, dispatch them, and then treat the
  // remainder as a single final data: field if it is non-empty.
  void flushStreamBuffer(ReplyState &state, const Token &token);

  // Returns true if `payload` parses as a single complete JSON value
  // (object or array). Used to detect truncated streams.
  static bool isCompleteJson(const QByteArray &payload);

  QNetworkAccessManager *m_networkManager = nullptr;
  QHash<Token, ReplyState> m_states;
  QHash<QNetworkReply *, Token> m_replyToToken;
};