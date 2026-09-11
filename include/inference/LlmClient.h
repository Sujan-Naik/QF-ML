#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

class LlmClient : public QObject {
    Q_OBJECT

public:
    explicit LlmClient(
        QNetworkAccessManager *networkManager,
        QObject *parent = nullptr
    );

    struct Request {
        QString url;
        QJsonArray messages;
        QString model = QStringLiteral("local-model");
        double temperature = 0.7;
        int timeoutMs = 120000;
        QString grammar = QString();
    };

    void sendRequest(const Request &request);

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

private:
    QNetworkAccessManager *m_networkManager = nullptr;
    QNetworkReply *m_currentReply = nullptr;

    QByteArray m_streamBuffer;
    QByteArray m_currentSseData;

    bool m_requestFailed = false;
    bool m_abortRequested = false;
};