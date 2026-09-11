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
    enum class AuthType {
        None,
        Bearer
    };

    struct Request {
        QString url;

        AuthType authType =
            AuthType::None;

        QString apiKey;

        QJsonArray messages;

        QString model =
            QStringLiteral("local-model");

        double temperature =
            0.7;

        int timeoutMs =
            120000;

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
    };

    explicit LlmClient(
        QNetworkAccessManager *networkManager,
        QObject *parent = nullptr
    );

    void sendRequest(
        const Request &request
    );

    void abortRequest();

    bool isActive() const;

signals:
    void deltaReceived(
        const QString &text
    );

    void requestFinished();

    void requestError(
        const QString &error
    );

private slots:
    void onReadyRead();

    void onFinished();

    void onError(
        QNetworkReply::NetworkError error
    );

private:
    void consumeStreamBuffer();

    void processSseLine(
        const QByteArray &line
    );

    void dispatchSseMessage(
        const QByteArray &rawPayload
    );

    QString buildReplyError(
        QNetworkReply *reply
    ) const;

private:
    QNetworkAccessManager *m_networkManager =
        nullptr;

    QNetworkReply *m_currentReply =
        nullptr;

    QByteArray m_streamBuffer;

    QByteArray m_currentSseData;

    bool m_requestFailed =
        false;

    bool m_abortRequested =
        false;
};